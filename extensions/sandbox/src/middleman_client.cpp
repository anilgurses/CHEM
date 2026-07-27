#include "middleman_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <regex>

#include "chem/common.h"
#include "chem/nlohmann/json.hpp"
#include "chem/runtime_config.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

namespace chem {

SandboxExtension::~SandboxExtension() { onStop(); }

void SandboxExtension::parseUrl(const std::string& url) {
    std::regex url_re(R"(https?://([^/:]+):?(\d*))");
    std::smatch match;
    if (std::regex_search(url, match, url_re)) {
        m_host = match[1].str();
        m_port = match[2].str().empty() ? "80" : match[2].str();
    } else {
        m_host = "localhost";
        m_port = "8080";
    }
}

bool SandboxExtension::onStart(const json& config) {
    if (m_running.load()) return true;

    if (config.contains("middlemanUrl") &&
        config["middlemanUrl"].is_string()) {
        m_serverUrl = config["middlemanUrl"].get<std::string>();
    }
    if (config.contains("updateRateMs") &&
        config["updateRateMs"].is_number()) {
        RuntimeConfig::sandbox_update_rate_ms.store(
            std::max(50, config["updateRateMs"].get<int>()));
    }

    if (m_serverUrl.empty()) {
        m_serverUrl = "http://localhost:8080";
    }
    parseUrl(m_serverUrl);

    logger = spdlog::get("CHEM");

    RuntimeConfig::sandbox_enabled.store(true);
    m_running.store(true);
    m_thread = std::thread(&SandboxExtension::pollLoop, this);

    LOG_INFO("SANDBOX",
             fmt::format("SandboxExtension started (middleman={})", m_serverUrl));
    return true;
}

void SandboxExtension::onStop() {
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
    RuntimeConfig::sandbox_enabled.store(false);
    LOG_INFO("SANDBOX", "SandboxExtension stopped");
}

json SandboxExtension::handleCommand(const std::string& action,
                                     const json& params) {
    json resp;
    if (action == "start") {
        bool ok = onStart(params);
        resp["status"] = ok ? "success" : "fail";
        resp["message"] = ok ? "Sandbox mode started" : "Failed to start";
        return resp;
    }
    if (action == "stop") {
        onStop();
        resp["status"] = "success";
        resp["message"] = "Sandbox mode stopped";
        return resp;
    }
    if (action == "status") {
        return getStatus();
    }
    resp["status"] = "fail";
    resp["message"] = "Unknown action: " + action;
    return resp;
}

json SandboxExtension::getStatus() const {
    json s;
    s["name"] = name();
    s["running"] = m_running.load();
    s["enabled"] = RuntimeConfig::sandbox_enabled.load();
    s["middlemanUrl"] = m_serverUrl;
    s["updateRateMs"] = RuntimeConfig::sandbox_update_rate_ms.load();
    return s;
}

json SandboxExtension::getConfigSchema() const {
    json schema;
    schema["middlemanUrl"] = "http://localhost:8080";
    schema["updateRateMs"] = 500;
    return schema;
}

std::string SandboxExtension::httpPost(const std::string& target,
                                       const std::string& body) {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const results = resolver.resolve(m_host, m_port);
    stream.connect(results);
    stream.expires_after(std::chrono::seconds(10));

    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host, m_host);
    req.set(http::field::user_agent, "CHEM-Sandbox/1.0");
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    return res.body();
}

void SandboxExtension::pollLoop() {
    while (m_running.load()) {
        try {
            pushPathLoss();
        } catch (const std::exception& e) {
            LOG_WARN("SANDBOX",
                     fmt::format("Middleman push error: {} — will retry",
                                 e.what()));
        }
        const int rate_ms =
            std::max(50, RuntimeConfig::sandbox_update_rate_ms.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(rate_ms));
    }
}

void SandboxExtension::pushPathLoss() {
    if (!m_intermediateMap || !m_nodeMap) return;

    json payload;
    json channels_arr = json::array();

    for (auto& [freq, intermediate] : *m_intermediateMap) {
        auto& channelList = intermediate->getChannelList();

        for (auto& [key, ch] : channelList) {
            const std::string& srcId = ch.getSrc();
            const std::string& destId = ch.getDest();

            if (srcId == destId) continue;

            auto resolveName = [this](const std::string& id) -> std::string {
                auto it = m_nodeMap->find(id);
                if (it != m_nodeMap->end()) {
                    const auto& n = it->second.getConfig().getName();
                    if (!n.empty()) return n;
                }
                return id;
            };

            json entry;
            entry["tx"] = resolveName(srcId);
            entry["rx"] = resolveName(destId);
            entry["freq_hz"] = freq;
            entry["pathloss_db"] = ch.getPlDb();
            entry["distance_m"] = ch.getLinkDistance();
            entry["elevation_deg"] = ch.getLinkElevation();

            channels_arr.push_back(std::move(entry));
        }
    }

    if (channels_arr.empty()) return;

    payload["channels"] = std::move(channels_arr);
    httpPost("/api/pathloss", payload.dump());
}

}  // namespace chem
