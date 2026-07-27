#include "sionna_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <cmath>
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

SionnaExtension::~SionnaExtension() { onStop(); }

void SionnaExtension::parseUrl(const std::string& url) {
    std::regex url_re(R"(https?://([^/:]+):?(\d*))");
    std::smatch match;
    if (std::regex_search(url, match, url_re)) {
        m_host = match[1].str();
        m_port = match[2].str().empty() ? "80" : match[2].str();
    } else {
        m_host = "localhost";
        m_port = "8000";
    }
}

// ---------------------------------------------------------------------------
// ChannelExtension interface
// ---------------------------------------------------------------------------

bool SionnaExtension::onStart(const json& config) {
    if (m_running.load()) return true;

    // Parse config
    if (config.contains("serverUrl") && config["serverUrl"].is_string()) {
        m_serverUrl = config["serverUrl"].get<std::string>();
    }
    if (config.contains("updateRateMs") && config["updateRateMs"].is_number()) {
        m_updateRateMs = std::max(50, config["updateRateMs"].get<int>());
    }
    if (config.contains("maxDepth") && config["maxDepth"].is_number()) {
        m_maxDepth = std::max(1, config["maxDepth"].get<int>());
    }
    if (config.contains("numSamples") && config["numSamples"].is_number()) {
        m_numSamples = std::max(1000, config["numSamples"].get<int>());
    }
    if (config.contains("sceneConfig") && config["sceneConfig"].is_string()) {
        m_sceneConfig = config["sceneConfig"].get<std::string>();
    }
    if (config.contains("referenceOrigin") &&
        config["referenceOrigin"].is_object()) {
        const auto& ref = config["referenceOrigin"];
        if (ref.contains("lat") && ref["lat"].is_number())
            m_refLat = ref["lat"].get<double>();
        if (ref.contains("lon") && ref["lon"].is_number())
            m_refLon = ref["lon"].get<double>();
        if (ref.contains("alt") && ref["alt"].is_number())
            m_refAlt = ref["alt"].get<double>();
    }
    if (config.contains("sceneOffset") && config["sceneOffset"].is_object()) {
        const auto& off = config["sceneOffset"];
        if (off.contains("x") && off["x"].is_number())
            m_offsetX = off["x"].get<double>();
        if (off.contains("y") && off["y"].is_number())
            m_offsetY = off["y"].get<double>();
        if (off.contains("z") && off["z"].is_number())
            m_offsetZ = off["z"].get<double>();
    }
    if (config.contains("scale") && config["scale"].is_number()) {
        m_scale = config["scale"].get<double>();
    }

    if (m_serverUrl.empty()) {
        m_serverUrl = "http://localhost:8000";
    }
    parseUrl(m_serverUrl);

    logger = spdlog::get("CHEM");

    m_running.store(true);
    m_thread = std::thread(&SionnaExtension::pollLoop, this);
    LOG_INFO("SIONNA",
             fmt::format("SionnaExtension started (server={})", m_serverUrl));
    return true;
}

void SionnaExtension::onStop() {
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();

    clearExtensionFlags();
    m_sceneId.clear();
    m_txMap.clear();
    m_rxMap.clear();
    m_txOrder.clear();
    m_rxOrder.clear();
    m_lastPositions.clear();
    m_avgCirResponseMs = 0.0;
    m_cirResponseCount = 0;
    LOG_INFO("SIONNA", "SionnaExtension stopped");
}

json SionnaExtension::handleCommand(const std::string& action,
                                    const json& params) {
    json resp;
    if (action == "start") {
        onStart(params);
        resp["status"] = "success";
        resp["message"] = "Sionna extension started";
    } else if (action == "stop") {
        onStop();
        resp["status"] = "success";
        resp["message"] = "Sionna extension stopped";
    } else if (action == "status") {
        resp = getStatus();
    } else if (action == "config") {
        // Update runtime config
        if (params.contains("updateRateMs") &&
            params["updateRateMs"].is_number()) {
            m_updateRateMs =
                std::max(50, params["updateRateMs"].get<int>());
        }
        if (params.contains("maxDepth") && params["maxDepth"].is_number()) {
            m_maxDepth = std::max(1, params["maxDepth"].get<int>());
        }
        if (params.contains("numSamples") && params["numSamples"].is_number()) {
            m_numSamples = std::max(1000, params["numSamples"].get<int>());
        }
        if (params.contains("sceneConfig") &&
            params["sceneConfig"].is_string()) {
            m_sceneConfig = params["sceneConfig"].get<std::string>();
        }
        if (params.contains("referenceOrigin") &&
            params["referenceOrigin"].is_object()) {
            const auto& ref = params["referenceOrigin"];
            if (ref.contains("lat") && ref["lat"].is_number())
                m_refLat = ref["lat"].get<double>();
            if (ref.contains("lon") && ref["lon"].is_number())
                m_refLon = ref["lon"].get<double>();
            if (ref.contains("alt") && ref["alt"].is_number())
                m_refAlt = ref["alt"].get<double>();
            // Push to Sionna if scene already exists
            if (!m_sceneId.empty()) {
                json origBody;
                origBody["lat"] = m_refLat;
                origBody["lon"] = m_refLon;
                origBody["alt"] = m_refAlt;
                try {
                    httpPut("/scenes/" + m_sceneId + "/update_origin",
                            origBody.dump());
                    LOG_INFO("SIONNA",
                             fmt::format("Updated scene origin to ({:.6f}, "
                                         "{:.6f}, {:.1f})",
                                         m_refLat, m_refLon, m_refAlt));
                } catch (const std::exception& e) {
                    LOG_WARN("SIONNA",
                             fmt::format("Failed to update scene origin: {}",
                                         e.what()));
                }
            }
        }
        if (params.contains("sceneOffset") &&
            params["sceneOffset"].is_object()) {
            const auto& off = params["sceneOffset"];
            if (off.contains("x") && off["x"].is_number())
                m_offsetX = off["x"].get<double>();
            if (off.contains("y") && off["y"].is_number())
                m_offsetY = off["y"].get<double>();
            if (off.contains("z") && off["z"].is_number())
                m_offsetZ = off["z"].get<double>();
        }
        if (params.contains("scale") && params["scale"].is_number()) {
            m_scale = params["scale"].get<double>();
        }
        resp["status"] = "success";
        resp["updateRateMs"] = m_updateRateMs;
        resp["maxDepth"] = m_maxDepth;
        resp["numSamples"] = m_numSamples;
        resp["sceneConfig"] = m_sceneConfig;
        resp["referenceOrigin"] = {
            {"lat", m_refLat}, {"lon", m_refLon}, {"alt", m_refAlt}};
        resp["sceneOffset"] = {
            {"x", m_offsetX}, {"y", m_offsetY}, {"z", m_offsetZ}};
        resp["scale"] = m_scale;
    } else {
        resp["status"] = "fail";
        resp["message"] = "Unknown action: " + action;
    }
    return resp;
}

json SionnaExtension::getStatus() const {
    json resp;
    resp["status"] = "success";
    resp["name"] = name();
    resp["running"] = m_running.load();
    resp["serverUrl"] = m_serverUrl;
    if (!m_sceneId.empty()) resp["sceneId"] = m_sceneId;
    resp["updateRateMs"] = m_updateRateMs;
    resp["maxDepth"] = m_maxDepth;
    resp["numSamples"] = m_numSamples;
    resp["sceneConfig"] = m_sceneConfig;
    resp["referenceOrigin"] = {
        {"lat", m_refLat},
        {"lon", m_refLon},
        {"alt", m_refAlt}
    };
    resp["sceneOffset"] = {
        {"x", m_offsetX}, {"y", m_offsetY}, {"z", m_offsetZ}};
    resp["scale"] = m_scale;
    if (!m_sceneAlignment.is_null()) {
        resp["sceneAlignment"] = m_sceneAlignment;
    }
    if (m_cirResponseCount > 0) {
        resp["avgCirResponseMs"] = std::round(m_avgCirResponseMs * 10.0) / 10.0;
    }
    return resp;
}

json SionnaExtension::getConfigSchema() const {
    return {
        {"serverUrl", {{"type", "string"}, {"default", "http://localhost:8000"}}},
        {"updateRateMs", {{"type", "integer"}, {"default", 500}, {"min", 50}}},
        {"maxDepth", {{"type", "integer"}, {"default", 3}, {"min", 1}}},
        {"numSamples", {{"type", "integer"}, {"default", 100000}, {"min", 1000}}},
        {"sceneConfig", {{"type", "string"}, {"default", "aerpaw"}}},
        {"referenceOrigin", {{"type", "object"}, {"properties", {
            {"lat", {{"type", "number"}, {"default", 35.72750947}}},
            {"lon", {{"type", "number"}, {"default", -78.69595819}}},
            {"alt", {{"type", "number"}, {"default", 112.0}}}
        }}}},
        {"sceneOffset", {{"type", "object"}, {"properties", {
            {"x", {{"type", "number"}, {"default", 118.1}}},
            {"y", {{"type", "number"}, {"default", -123.4}}},
            {"z", {{"type", "number"}, {"default", 0.0}}}
        }}}},
        {"scale", {{"type", "number"}, {"default", 1.0}}}
    };
}

// Extension flag management

void SionnaExtension::clearExtensionFlags() {
    if (!m_intermediateMap) return;
    for (auto& [freq, intermediate] : *m_intermediateMap) {
        auto& channels = intermediate->getChannelList();
        for (auto& [key, ch] : channels) {
            if (ch.getActiveExtension() == "sionna") {
                ch.clearActiveExtension();
            }
        }
    }
}

// HTTP helpers

std::string SionnaExtension::httpGet(const std::string& target) {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const results = resolver.resolve(m_host, m_port);
    stream.connect(results);
    stream.expires_after(std::chrono::seconds(5));

    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host, m_host);
    req.set(http::field::user_agent, "CHEM-SionnaClient/1.0");

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    return res.body();
}

std::string SionnaExtension::httpPost(const std::string& target,
                                      const std::string& body) {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const results = resolver.resolve(m_host, m_port);
    stream.connect(results);
    stream.expires_after(std::chrono::seconds(10));

    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host, m_host);
    req.set(http::field::user_agent, "CHEM-SionnaClient/1.0");
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

std::string SionnaExtension::httpPut(const std::string& target,
                                     const std::string& body) {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const results = resolver.resolve(m_host, m_port);
    stream.connect(results);
    stream.expires_after(std::chrono::seconds(5));

    http::request<http::string_body> req{http::verb::put, target, 11};
    req.set(http::field::host, m_host);
    req.set(http::field::user_agent, "CHEM-SionnaClient/1.0");
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

// Link-budget helpers

double SionnaExtension::destSampleRate(const std::string& destId,
                                       double fallback) const {
    if (!m_nodeMap) return fallback;
    auto it = m_nodeMap->find(destId);
    if (it == m_nodeMap->end()) return fallback;
    const double rx = it->second.getConfig().getSampleRate().getRxRate();
    return rx > 0.0 ? rx : fallback;
}

// CIR -> FIR tap conversion

chem::signal_v SionnaExtension::cirToTaps(
    const std::vector<double>& delays, const std::vector<double>& gains_re,
    const std::vector<double>& gains_im, double sampleRate) const {
    const int max_taps = std::max(0, RuntimeConfig::cir_max_taps.load());
    const size_t numPaths =
        std::min({delays.size(), gains_re.size(), gains_im.size()});

    size_t maxIdx = 0;
    for (size_t i = 0; i < numPaths; ++i) {
        size_t idx = static_cast<size_t>(std::round(delays[i] * sampleRate));
        if (max_taps > 0 && idx >= static_cast<size_t>(max_taps)) continue;
        maxIdx = std::max(maxIdx, idx);
    }

    chem::signal_v taps(maxIdx + 1, fc(0.0f, 0.0f));

    for (size_t i = 0; i < numPaths; ++i) {
        size_t idx = static_cast<size_t>(std::round(delays[i] * sampleRate));
        if (max_taps > 0 && idx >= static_cast<size_t>(max_taps)) continue;
        if (idx < taps.size()) {
            taps[idx] += fc(static_cast<float>(gains_re[i]),
                            static_cast<float>(gains_im[i]));
        }
    }

    return taps;
}

// Scene creation

bool SionnaExtension::createScene() {
    try {
        json createBody;
        createBody["scene_config"] = m_sceneConfig;
        createBody["scene_origin"] = {
            {"lat", m_refLat},
            {"lon", m_refLon},
            {"alt", m_refAlt}
        };
        createBody["scene_offset"] = {
            {"x", m_offsetX},
            {"y", m_offsetY},
            {"z", m_offsetZ}
        };
        createBody["scale"] = m_scale;

        std::string response = httpPost("/scenes", createBody.dump());
        json respJson = json::parse(response);
        if (!respJson.contains("scene_id")) {
            LOG_WARN("SIONNA", "Scene creation response missing scene_id");
            return false;
        }
        m_sceneId = respJson["scene_id"].get<std::string>();
        LOG_INFO("SIONNA",
                 fmt::format("Created scene: {} (config='{}', origin: {:.6f}, "
                             "{:.6f}, {:.1f} HAE, offset: {:.1f}, {:.1f}, "
                             "{:.1f}, scale: {:.3f})",
                             m_sceneId, m_sceneConfig, m_refLat, m_refLon,
                             m_refAlt, m_offsetX, m_offsetY, m_offsetZ,
                             m_scale));
    } catch (const std::exception& e) {
        LOG_WARN("SIONNA",
                 fmt::format("Failed to create scene: {}", e.what()));
        return false;
    }

    fetchSceneAlignment();

    return true;
}

void SionnaExtension::fetchSceneAlignment() {
    if (m_sceneId.empty()) return;
    try {
        std::string response = httpGet("/scenes/" + m_sceneId);
        json info = json::parse(response);

        json alignment;
        for (const char* key :
             {"scene_config", "scene_path", "offset", "scale", "units"}) {
            if (info.contains(key)) alignment[key] = info[key];
        }
        m_sceneAlignment = alignment;

        LOG_INFO("SIONNA",
                 fmt::format("Scene alignment confirmed: {}", alignment.dump()));
    } catch (const std::exception& e) {
        LOG_WARN("SIONNA",
                 fmt::format("Failed to fetch scene alignment: {}", e.what()));
    }
}

// Poll loop

void SionnaExtension::pollLoop() {
    // Create a scene before starting the sync loop
    while (m_running.load() && m_sceneId.empty()) {
        if (createScene()) break;
        LOG_WARN("SIONNA", "Retrying scene creation in 2s...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    while (m_running.load()) {
        try {
            syncPositions();
            computeAndApplyCIR();
        } catch (const std::exception& e) {
            LOG_WARN("SIONNA",
                     fmt::format("Poll loop error: {} — retaining last CIR",
                                 e.what()));
        }

        const int rate_ms = std::max(50, m_updateRateMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(rate_ms));
    }
}

// Position sync

void SionnaExtension::syncPositions() {
    if (m_sceneId.empty() || !m_nodeMap) return;

    const std::string txBase = "/scenes/" + m_sceneId + "/transmitters";
    const std::string rxBase = "/scenes/" + m_sceneId + "/receivers";

    for (const auto& [nodeId, node] : *m_nodeMap) {
        auto locOpt = node.getLocation();
        if (!locOpt.has_value()) continue;

        const auto& loc = locOpt->position;

        // Check if position actually changed
        auto cachedIt = m_lastPositions.find(nodeId);
        if (cachedIt != m_lastPositions.end()) {
            const auto& c = cachedIt->second;
            if (c.lat == loc.lat && c.lon == loc.lon && c.alt == loc.alt)
                continue;
        }

        const std::string& nodeName = node.getConfig().getName().empty()
                                          ? nodeId
                                          : node.getConfig().getName();

        // The server expects position alt in the same HAE datum as
        // scene_origin.alt (ground level). CHEM node altitudes come from the
        // vehicle's relative_altitude_m (AGL, height above the takeoff/home
        // point), so shift them onto the HAE ground reference: a device on the
        // ground (AGL 0) maps to scene_origin.alt, a drone 35m up maps to
        // scene_origin.alt + 35.
        const double haeAlt = static_cast<double>(loc.alt) + m_refAlt;

        // Build GeoPosition object for the API
        json posObj;
        posObj["lat"] = loc.lat;
        posObj["lon"] = loc.lon;
        posObj["alt"] = haeAlt;

        // Sionna requires unique names across TX and RX, so suffix them
        const std::string txName = nodeName + "-tx";
        const std::string rxName = nodeName + "-rx";

        // Register as TX if not already registered
        if (m_txMap.find(nodeId) == m_txMap.end()) {
            json txBody;
            txBody["name"] = txName;
            txBody["position"] = posObj;
            // Keep Sionna normalized; CHEM applies device/PA/cable gains
            // locally in Channel::processChannel().
            txBody["signal_power"] = 0.0;
            txBody["velocity"] = {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}};
            try {
                httpPost(txBase, txBody.dump());
                m_txMap[nodeId] = txName;
                m_txOrder.push_back(txName);
                LOG_INFO("SIONNA",
                         fmt::format(
                             "Registered TX '{}' at ({:.6f}, {:.6f}, {:.1f} HAE)",
                             txName, loc.lat, loc.lon, haeAlt));
            } catch (const std::exception& e) {
                LOG_WARN("SIONNA", fmt::format("Failed to register TX '{}': {}",
                                               txName, e.what()));
            }
        } else {
            json updateBody;
            updateBody["position"] = posObj;
            try {
                httpPut(txBase + "/" + m_txMap[nodeId], updateBody.dump());
            } catch (const std::exception& e) {
                LOG_WARN("SIONNA", fmt::format("Failed to update TX '{}': {}",
                                               txName, e.what()));
            }
        }

        // Register as RX if not already registered
        if (m_rxMap.find(nodeId) == m_rxMap.end()) {
            json rxBody;
            rxBody["name"] = rxName;
            rxBody["position"] = posObj;
            rxBody["velocity"] = {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}};
            try {
                httpPost(rxBase, rxBody.dump());
                m_rxMap[nodeId] = rxName;
                m_rxOrder.push_back(rxName);
                LOG_INFO("SIONNA",
                         fmt::format(
                             "Registered RX '{}' at ({:.6f}, {:.6f}, {:.1f} HAE)",
                             rxName, loc.lat, loc.lon, haeAlt));
            } catch (const std::exception& e) {
                LOG_WARN("SIONNA", fmt::format("Failed to register RX '{}': {}",
                                               rxName, e.what()));
            }
        } else {
            json updateBody;
            updateBody["position"] = posObj;
            try {
                httpPut(rxBase + "/" + m_rxMap[nodeId], updateBody.dump());
            } catch (const std::exception& e) {
                LOG_WARN("SIONNA", fmt::format("Failed to update RX '{}': {}",
                                               rxName, e.what()));
            }
        }

        m_lastPositions[nodeId] = {loc.lat, loc.lon, loc.alt};
    }
}

// Compute and apply CIR from Sionna

void SionnaExtension::computeAndApplyCIR() {
    if (m_sceneId.empty() || !m_intermediateMap) return;
    if (m_txMap.size() < 1 || m_rxMap.size() < 1) return;

    // Only query if at least one intermediate has active channel links
    bool hasLinks = false;
    for (const auto& [freq, intermediate] : *m_intermediateMap) {
        if (!intermediate->getChannelList().empty()) {
            hasLinks = true;
            break;
        }
    }
    if (!hasLinks) return;

    const std::string pathsUrl = "/scenes/" + m_sceneId + "/simulation/paths";
    const std::string cirUrl = "/scenes/" + m_sceneId + "/simulation/cir";

    json pathReq;
    pathReq["max_depth"] = m_maxDepth;
    pathReq["num_samples"] = m_numSamples;

    auto t0 = std::chrono::steady_clock::now();

    try {
        httpPost(pathsUrl, pathReq.dump());
    } catch (const std::exception& e) {
        LOG_WARN("SIONNA",
                 fmt::format("Failed to compute paths: {}", e.what()));
        return;
    }

    std::string cirResponse;
    try {
        cirResponse = httpGet(cirUrl);
    } catch (const std::exception& e) {
        LOG_WARN("SIONNA", fmt::format("Failed to fetch CIR: {}", e.what()));
        return;
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // EWMA for smoothing as it might fluctuate like crazy (alpha = 0.2)
    // I love EWMA 
    if (m_cirResponseCount == 0) {
        m_avgCirResponseMs = elapsedMs;
    } else {
        m_avgCirResponseMs = 0.8 * m_avgCirResponseMs + 0.2 * elapsedMs;
    }
    ++m_cirResponseCount;

    json cirJson;
    try {
        cirJson = json::parse(cirResponse);
    } catch (const std::exception& e) {
        LOG_WARN("SIONNA",
                 fmt::format("Failed to parse CIR JSON: {}", e.what()));
        return;
    }

    if (!cirJson.contains("delays") || !cirJson.contains("gains") ||
        !cirJson.contains("shape"))
        return;

    const auto& shape = cirJson["shape"];
    const int numRx = shape["num_rx"].get<int>();
    const int numTx = shape["num_tx"].get<int>();
    const int numPaths = shape["num_paths"].get<int>();

    if (numPaths == 0) return;

    const auto& delaysJson = cirJson["delays"];
    const auto& gainsRealJson = cirJson["gains"]["real"];
    const auto& gainsImagJson = cirJson["gains"]["imag"];

    // Build reverse lookup: sionna name -> CHEM nodeId
    std::map<std::string, std::string> txNameToId;
    std::map<std::string, std::string> rxNameToId;
    for (const auto& [nodeId, sionnaName] : m_txMap)
        txNameToId[sionnaName] = nodeId;
    for (const auto& [nodeId, sionnaName] : m_rxMap)
        rxNameToId[sionnaName] = nodeId;

    // Iterate over TX-RX pairs using registration order indices
    for (int rxIdx = 0;
         rxIdx < numRx && rxIdx < static_cast<int>(m_rxOrder.size());
         ++rxIdx) {
        for (int txIdx = 0;
             txIdx < numTx && txIdx < static_cast<int>(m_txOrder.size());
             ++txIdx) {
            const std::string& txName = m_txOrder[txIdx];
            const std::string& rxName = m_rxOrder[rxIdx];

            auto txIt = txNameToId.find(txName);
            auto rxIt = rxNameToId.find(rxName);
            if (txIt == txNameToId.end() || rxIt == rxNameToId.end()) continue;

            const std::string& srcId = txIt->second;
            const std::string& destId = rxIt->second;

            // Skip self-links
            if (srcId == destId) continue;

            std::vector<double> delays;
            std::vector<double> gains_re;
            std::vector<double> gains_im;

            try {
                const auto& pairDelays = delaysJson[rxIdx][txIdx];
                const auto& pairGainsRe = gainsRealJson[rxIdx][0][txIdx][0];
                const auto& pairGainsIm = gainsImagJson[rxIdx][0][txIdx][0];

                for (int p = 0; p < numPaths; ++p) {
                    delays.push_back(pairDelays[p].get<double>());
                    if (pairGainsRe[p].is_array()) {
                        gains_re.push_back(pairGainsRe[p][0].get<double>());
                        gains_im.push_back(pairGainsIm[p][0].get<double>());
                    } else {
                        gains_re.push_back(pairGainsRe[p].get<double>());
                        gains_im.push_back(pairGainsIm[p].get<double>());
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN(
                    "SIONNA",
                    fmt::format(
                        "Failed to extract CIR for TX '{}' -> RX '{}': {}",
                        txName, rxName, e.what()));
                continue;
            }

            if (delays.empty()) continue;

            const chId ch{0, 0};
            const double sampleRate = destSampleRate(destId, 1e6);
            for (auto& [freq, intermediate] : *m_intermediateMap) {
                auto taps =
                    cirToTaps(delays, gains_re, gains_im, sampleRate);
                if (taps.empty()) continue;

                bool updated =
                    intermediate->updateCIR(srcId, destId, ch, taps);
                if (updated) {
                    auto chIt = intermediate->getChannel(srcId, destId);
                    auto& channelList = intermediate->getChannelList();
                    if (chIt != channelList.end()) {
                        chIt->second.setActiveExtension("sionna", true);
                    }
                }
            }
        }
    }
}

}  // namespace chem
