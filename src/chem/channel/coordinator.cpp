#include "chem/channel/coordinator.h"

#include <boost/asio.hpp>
#include <boost/geometry.hpp>
#if __has_include(<boost/geometry/algorithms/detail/azimuth.hpp>)
#include <boost/geometry/algorithms/detail/azimuth.hpp>
#elif __has_include(<boost/geometry/algorithms/azimuth.hpp>)
#include <boost/geometry/algorithms/azimuth.hpp>
#else
#error "Boost.Geometry azimuth header not found"
#endif
#include <spdlog/fmt/fmt.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <thread>

#include "chem/antennas/antennas.h"
#include "chem/common.h"
#include "chem/db/dbHandler.h"
#include "chem/runtime_config.h"
#include "sionna_client.h"

using namespace chem;
using namespace chem::channel;
using namespace boost::asio;

// TODO: refactor here
const std::string first_node_token = "-CH";
const std::string second_node_token = "==>";

using namespace std::chrono_literals;

std::atomic<bool> running(true);

double shadow_std = SHADOWING_STD;
std::map<double, double> shadow_std_by_freq;

int _tmp_dist = 0;

static const auto g_proc_start = std::chrono::steady_clock::now();

static std::string resolveNodeId(
    const std::map<std::string, chem::Node>& nodeMap, const std::string& raw) {
    auto it = nodeMap.find(raw);
    if (it != nodeMap.end()) return raw;

    for (const auto& pair : nodeMap) {
        const auto& cfg = pair.second.getConfig();
        if (cfg.getName() == raw) {
            return cfg.getId();
        }
    }
    return raw;
}

// TODO: Convert these std::map's to struct
Coordinator::Coordinator(
    const std::string& ipv4addr, const uint16_t& portNum,
    std::map<std::string, Node>& nodeMap,
    std::map<double, std::shared_ptr<Intermediate>>& intermediate_map,
    std::shared_ptr<chem::PropagationDefaults> propagationDefaults)
    : m_ipAddress(ipv4addr),
      m_portNum(portNum),
      m_nodeMap(nodeMap),
      m_intermediateMap(intermediate_map),
      m_propagationDefaults(std::move(propagationDefaults)),
      m_tcpServer() {
    // Extensions
    m_extensionRegistry.setContext(m_nodeMap, m_intermediateMap);
    m_extensionRegistry.registerExtension(
        std::make_unique<chem::SionnaExtension>());
}

Coordinator::~Coordinator() {}

void Coordinator::start() {
    LOG_INFO("COORDINATOR",
             fmt::format("Channel Coordinator started listening on {}:{}",
                         m_ipAddress, COORD_PORT_NUM));

    m_tcpServer.Bind(
        m_ipAddress, COORD_PORT_NUM,
        [self = shared_from_this()](const std::string& req, std::string& resp) {
            self->requestHandler(req, resp);
        });

    m_tcpServer.startAccept();
    std::thread ch_updater(&Coordinator::updateChannels, this);
    ch_updater.detach();

    m_tcpServer.run();
    running = false;
}

void Coordinator::requestHandler(const std::string& request,
                                 std::string& response) {
    json coord_info;
    json resp;

    try {
        coord_info = json::parse(request.data());
    } catch (std::exception& e) {
        LOG_WARN("COORDINATOR",
                 fmt::format("Couldn't parse the received message. Detail : {}",
                             e.what()));
        resp["status"] = "fail";
        response = resp.dump();
        return;
    }

    LOG_DEBUG("COOORDINATOR",
              fmt::format("Received a request {}", coord_info.dump()));

    if (coord_info["CMD"] == "GET_NODES") {
        std::string _nodes = getNodes();

        response = _nodes;
    } else if (coord_info["CMD"] == "GET_CHANNELS") {
        std::string _chs = getChannels();

        response = _chs;
    } else if (coord_info["CMD"] == "GET_IND_CHANNELS") {
        std::string _chs = getIndividualChannels();

        response = _chs;
    } else if (coord_info["CMD"] == "GET_ANTENNA_TYPES") {
        response = getAntennaTypes();
    } else if (coord_info["CMD"] == "GET_CONFIG") {
        json cfg;
        cfg["status"] = "success";
        cfg["vehiclePollRateMs"] =
            chem::RuntimeConfig::vehicle_poll_rate_ms.load();
        cfg["channelUpdateRateMs"] =
            chem::RuntimeConfig::channel_update_rate_ms.load();
        cfg["cirMaxTaps"] = chem::RuntimeConfig::cir_max_taps.load();
        cfg["extensions"] = m_extensionRegistry.getAllStatus();
        response = cfg.dump();
    } else if (coord_info["CMD"] == "SET_CONFIG") {
        auto clamp_ms = [](int v, int lo, int hi) -> int {
            return std::max(lo, std::min(hi, v));
        };
        if (coord_info.contains("vehiclePollRateMs") &&
            coord_info["vehiclePollRateMs"].is_number()) {
            const int v = coord_info["vehiclePollRateMs"];
            chem::RuntimeConfig::vehicle_poll_rate_ms.store(
                clamp_ms(v, 20, 60'000));
        }
        if (coord_info.contains("channelUpdateRateMs") &&
            coord_info["channelUpdateRateMs"].is_number()) {
            const int v = coord_info["channelUpdateRateMs"];
            chem::RuntimeConfig::channel_update_rate_ms.store(
                clamp_ms(v, 1, 60'000));
        }
        if (coord_info.contains("cirMaxTaps") &&
            coord_info["cirMaxTaps"].is_number()) {
            const int v = coord_info["cirMaxTaps"];
            chem::RuntimeConfig::cir_max_taps.store(
                std::max(0, std::min(8192, v)));
        }
        json cfg;
        cfg["status"] = "success";
        cfg["vehiclePollRateMs"] =
            chem::RuntimeConfig::vehicle_poll_rate_ms.load();
        cfg["channelUpdateRateMs"] =
            chem::RuntimeConfig::channel_update_rate_ms.load();
        cfg["cirMaxTaps"] = chem::RuntimeConfig::cir_max_taps.load();
        cfg["extensions"] = m_extensionRegistry.getAllStatus();
        response = cfg.dump();
    } else if (coord_info["CMD"] == "GET_STATUS") {
        response = getStatus();
    } else if (coord_info["CMD"] == "GET_ANTENNA_PATTERNS") {
        response = getAntennaPatterns();
    } else if (coord_info["CMD"] == "CHG_ANTENNA") {
        const std::string node = coord_info.value("node", "");
        const std::string pattern = coord_info.value("pattern", "");
        const std::string tx_pattern = coord_info.value("tx_pattern", "");
        const std::string rx_pattern = coord_info.value("rx_pattern", "");

        // Support both unified pattern and separate tx/rx patterns
        const bool has_unified = !pattern.empty();
        const bool has_separate = !tx_pattern.empty() || !rx_pattern.empty();

        if (node.empty() || (!has_unified && !has_separate)) {
            resp["status"] = "fail";
            resp["message"] =
                "Missing node or pattern (use 'pattern' for both, or "
                "'tx_pattern'/'rx_pattern' separately)";
            response = resp.dump();
            return;
        }

        auto* db = chem::DBHandler::GetInstance("");
        const std::string db_pattern =
            has_unified ? pattern
                        : (tx_pattern.empty() ? rx_pattern : tx_pattern);
        const bool db_ok = db->ChangeAntenna(node, db_pattern);

        bool mem_ok = false;
        for (auto& [id, nodeRef] : m_nodeMap) {
            auto& cfg = nodeRef.getConfig();
            if (cfg.getName() == node || id == node) {
                if (has_unified) {
                    cfg.setAntennaPattern(pattern);
                } else {
                    if (!tx_pattern.empty())
                        cfg.setTxAntennaPattern(tx_pattern);
                    if (!rx_pattern.empty())
                        cfg.setRxAntennaPattern(rx_pattern);
                }
                mem_ok = true;
                break;
            }
        }

        // Also update Receiver/Transmitter objects in all Intermediates
        bool interm_ok = false;
        for (auto& [freq, intermediate] : m_intermediateMap) {
            if (intermediate->updateNodeAntenna(node, pattern, tx_pattern,
                                                rx_pattern)) {
                interm_ok = true;
            }
        }

        resp["status"] =
            (db_ok && mem_ok && interm_ok)
                ? "success"
                : ((db_ok || mem_ok || interm_ok) ? "partial" : "fail");
        if (!mem_ok && !interm_ok) {
            resp["warning"] = "Node not found in memory, only database updated";
        }
        LOG_INFO(
            "COORDINATOR",
            fmt::format(
                "Antenna change: node='{}' pattern='{}' tx='{}' rx='{}' db={} "
                "mem={} interm={}",
                node, pattern, tx_pattern, rx_pattern, db_ok ? "ok" : "fail",
                mem_ok ? "ok" : "fail", interm_ok ? "ok" : "fail"));
        response = resp.dump();
    } else if (coord_info["CMD"] == "CHG_SOURCE_POWER") {
        const std::string node = coord_info.value("node", "");
        if (node.empty()) {
            resp["status"] = "fail";
            resp["message"] = "Missing 'node' field";
            response = resp.dump();
            return;
        }

        const std::string nodeId = resolveNodeId(m_nodeMap, node);
        auto nodeIt = m_nodeMap.find(nodeId);
        if (nodeIt == m_nodeMap.end()) {
            resp["status"] = "fail";
            resp["message"] = "Node not found";
            response = resp.dump();
            return;
        }

        auto& cfg = nodeIt->second.getConfig();
        if (!cfg.hasCharacteristics()) {
            resp["status"] = "fail";
            resp["message"] = "Node has no characteristics";
            response = resp.dump();
            return;
        }

        auto chars = cfg.getCharacteristics().value();
        if (coord_info.contains("sourcePowerDbfs") &&
            coord_info["sourcePowerDbfs"].is_number()) {
            chars.source_power_dbfs =
                coord_info["sourcePowerDbfs"].get<double>();
        } else {
            // Setting to null or omitting clears it (reverts to measured power)
            chars.source_power_dbfs = std::numeric_limits<double>::quiet_NaN();
        }
        cfg.setCharacteristics(chars);

        resp["status"] = "success";
        resp["sourcePowerDbfs"] = std::isfinite(chars.source_power_dbfs)
                                      ? json(chars.source_power_dbfs)
                                      : json(nullptr);
        LOG_INFO("COORDINATOR",
                 fmt::format(
                     "Source power changed: node='{}' sourcePowerDbfs={}", node,
                     std::isfinite(chars.source_power_dbfs)
                         ? std::to_string(chars.source_power_dbfs)
                         : "NaN (measured)"));
        response = resp.dump();
    } else if (coord_info["CMD"] == "CHG_PL") {
        const double freq = coord_info["freq"];
        const std::string plMode = coord_info["plMode"];

        float grCoeff = -1.0f;
        if (coord_info.contains("groundCoeff") &&
            coord_info["groundCoeff"].is_number()) {
            grCoeff = coord_info["groundCoeff"];
        }

        std::string scenario = "UMa";
        if (coord_info.contains("scenario") &&
            coord_info["scenario"].is_string()) {
            scenario = coord_info["scenario"];
        }

        response = setPathLossMode(freq * 1000000, plMode, grCoeff, scenario);

        // Apply model-specific parameters
        const double freq_hz = freq * 1000000;
        auto interm_it = m_intermediateMap.find(freq_hz);
        if (interm_it != m_intermediateMap.end()) {
            auto model = PropagationModelFromString(plMode);
            if (model == PropagationModel::OKUMURA_HATA) {
                std::string env = "URBAN";
                if (coord_info.contains("environment") &&
                    coord_info["environment"].is_string()) {
                    env = coord_info["environment"];
                }
                interm_it->second->setHataEnvironment(env);
            } else if (model == PropagationModel::LONGLEY_RICE) {
                float refractivity = 301.0f;
                float groundCond = 0.005f;
                float groundPerm = 15.0f;
                int climateZone = 5;
                if (coord_info.contains("refractivity") &&
                    coord_info["refractivity"].is_number())
                    refractivity = coord_info["refractivity"];
                if (coord_info.contains("groundConductivity") &&
                    coord_info["groundConductivity"].is_number())
                    groundCond = coord_info["groundConductivity"];
                if (coord_info.contains("groundPermittivity") &&
                    coord_info["groundPermittivity"].is_number())
                    groundPerm = coord_info["groundPermittivity"];
                if (coord_info.contains("climateZone") &&
                    coord_info["climateZone"].is_number())
                    climateZone = coord_info["climateZone"];
                interm_it->second->setITMParams(refractivity, groundCond,
                                                groundPerm, climateZone);
            }
        }
    } else if (coord_info["CMD"] == "SET_DEFAULT_PL") {
        const std::string plMode = coord_info.value("plMode", "FREE_SPACE");
        float grCoeff = -1.0f;
        if (coord_info.contains("groundCoeff") &&
            coord_info["groundCoeff"].is_number()) {
            grCoeff = coord_info["groundCoeff"];
        }
        std::string scenario = "UMa";
        if (coord_info.contains("scenario") &&
            coord_info["scenario"].is_string()) {
            scenario = coord_info["scenario"];
        }

        auto model = PropagationModelFromString(plMode);
        if (model == PropagationModel::UNKNOWN) {
            resp["status"] = "fail";
            resp["message"] = "Unknown propagation model";
            response = resp.dump();
        } else {
            // Parse model-specific parameters
            std::string environment = "URBAN";
            if (coord_info.contains("environment") &&
                coord_info["environment"].is_string())
                environment = coord_info["environment"];
            float refractivity = 301.0f;
            if (coord_info.contains("refractivity") &&
                coord_info["refractivity"].is_number())
                refractivity = coord_info["refractivity"];
            float groundCond = 0.005f;
            if (coord_info.contains("groundConductivity") &&
                coord_info["groundConductivity"].is_number())
                groundCond = coord_info["groundConductivity"];
            float groundPerm = 15.0f;
            if (coord_info.contains("groundPermittivity") &&
                coord_info["groundPermittivity"].is_number())
                groundPerm = coord_info["groundPermittivity"];
            int climateZone = 5;
            if (coord_info.contains("climateZone") &&
                coord_info["climateZone"].is_number())
                climateZone = coord_info["climateZone"];

            if (m_propagationDefaults) {
                m_propagationDefaults->model.store(model);
                m_propagationDefaults->groundCoeff.store(grCoeff);
                {
                    std::lock_guard<std::mutex> lock(
                        m_propagationDefaults->scenarioMutex);
                    m_propagationDefaults->scenario = scenario;
                    m_propagationDefaults->hataEnvironment = environment;
                }
                m_propagationDefaults->refractivity.store(refractivity);
                m_propagationDefaults->groundConductivity.store(groundCond);
                m_propagationDefaults->groundPermittivity.store(groundPerm);
                m_propagationDefaults->climateZone.store(climateZone);
            }

            // Apply to all existing intermediates
            for (auto& [freq_hz, interm] : m_intermediateMap) {
                interm->updatePathLoss(plMode, grCoeff);
                if (model == PropagationModel::THREE_GPP_38_901) {
                    interm->set3gppScenario(scenario);
                }
                if (model == PropagationModel::OKUMURA_HATA) {
                    interm->setHataEnvironment(environment);
                }
                if (model == PropagationModel::LONGLEY_RICE) {
                    interm->setITMParams(refractivity, groundCond, groundPerm,
                                         climateZone);
                }
            }

            resp["status"] = "success";
            resp["model"] = PropagationModelToString(model);
            response = resp.dump();

            LOG_INFO("COORDINATOR", fmt::format("Default propagation set to {} "
                                                "(groundCoeff={}, scenario={})",
                                                PropagationModelToString(model),
                                                grCoeff, scenario));
        }
    } else if (coord_info["CMD"] == "CHG_COEFF") {
        const double freq = coord_info["freq"];
        const std::string src = coord_info["src"];
        const std::string dest = coord_info["dest"];
        const double coeff = coord_info["coeff"];

        chId t_ch;
        t_ch.src = coord_info["p_src"];
        t_ch.dest = coord_info["p_dest"];

        response = setChCoeff(freq * 1000000, src, dest, t_ch, coeff);
    } else if (coord_info["CMD"] == "CHG_NOISE_MDL") {
        const double freq = coord_info["freq"];
        const std::string noiseMdl = coord_info["noiseModel"];
        const double snr = coord_info["pwr"];
        const std::string src = coord_info["src"];
        const std::string dest = coord_info["dest"];

        response = setNoiseModel(freq * 1000000, src, dest, noiseMdl, snr);
    } else if (coord_info["CMD"] == "CHG_FREQ_OFFSET") {
        const double freq = coord_info["freq"];
        const std::string src = coord_info["src"];
        const std::string dest = coord_info["dest"];
        const double offset_hz = coord_info["offsetHz"];
        response = setFrequencyOffset(freq * 1000000, src, dest, offset_hz);
    } else if (coord_info["CMD"] == "CHG_DOPPLER") {
        const double freq = coord_info["freq"];
        const std::string src = coord_info["src"];
        const std::string dest = coord_info["dest"];
        const bool enabled = coord_info.value("enabled", false);
        response = setDopplerEnabled(freq * 1000000, src, dest, enabled);
    } else if (coord_info["CMD"] == "CHG_CIR") {
        const double freq = coord_info["freq"];
        const std::string src = coord_info["src"];
        const std::string dest = coord_info["dest"];

        chId t_ch;
        t_ch.src = coord_info.value("p_src", 0);
        t_ch.dest = coord_info.value("p_dest", 0);

        chem::signal_v taps;
        if (coord_info.contains("taps") && coord_info["taps"].is_array()) {
            const int max_taps =
                std::max(0, chem::RuntimeConfig::cir_max_taps.load());
            for (const auto& t : coord_info["taps"]) {
                if (max_taps > 0 && static_cast<int>(taps.size()) >= max_taps)
                    break;
                if (t.is_number()) {
                    taps.emplace_back(static_cast<float>(t.get<double>()),
                                      0.0f);
                    continue;
                }
                if (t.is_array()) {
                    float re = 0.0f;
                    float im = 0.0f;
                    if (t.size() >= 1 && t[0].is_number())
                        re = static_cast<float>(t[0].get<double>());
                    if (t.size() >= 2 && t[1].is_number())
                        im = static_cast<float>(t[1].get<double>());
                    taps.emplace_back(re, im);
                    continue;
                }
                if (t.is_object()) {
                    float re = 0.0f;
                    float im = 0.0f;
                    if (t.contains("re") && t["re"].is_number())
                        re = static_cast<float>(t["re"].get<double>());
                    if (t.contains("im") && t["im"].is_number())
                        im = static_cast<float>(t["im"].get<double>());
                    taps.emplace_back(re, im);
                    continue;
                }
            }
        }

        response = setCIR(freq * 1000000, src, dest, t_ch, taps);
    } else if (coord_info["CMD"] == "UPD_SHADOW_STD") {
        const double std = coord_info["std"];
        if (coord_info.contains("freq")) {
            const double freq = coord_info["freq"];
            const double freq_hz = freq * 1000000;
            shadow_std_by_freq[freq_hz] = std;

            auto it = m_intermediateMap.find(freq_hz);
            if (it != m_intermediateMap.end()) {
                it->second->setShadowingSTD(std);
            }
        } else {
            shadow_std = std;
            setShadowingSTD();
        }
        resp["status"] = "success";

        response = resp.dump();
    } else if (coord_info["CMD"] == "GET_SHADOW_STD") {
        resp["status"] = "success";
        if (coord_info.contains("freq")) {
            const double freq = coord_info["freq"];
            const double freq_hz = freq * 1000000;
            auto it = shadow_std_by_freq.find(freq_hz);
            resp["std"] =
                (it != shadow_std_by_freq.end()) ? it->second : shadow_std;
        } else {
            resp["std"] = shadow_std;
        }

        response = resp.dump();
    } else if (coord_info["CMD"] == "EXT") {
        const std::string extName = coord_info.value("extension", "");
        const std::string action = coord_info.value("action", "");
        json params = coord_info.contains("params") ? coord_info["params"] : json::object();
        // Merge top-level keys into params for convenience
        for (auto& [k, v] : coord_info.items()) {
            if (k != "CMD" && k != "extension" && k != "action" && k != "params") {
                params[k] = v;
            }
        }
        resp = m_extensionRegistry.routeCommand(extName, action, params);
        response = resp.dump();
    } else if (coord_info["CMD"] == "EXT_LIST") {
        resp["status"] = "success";
        resp["extensions"] = m_extensionRegistry.getAllStatus();
        response = resp.dump();
    } else if (coord_info["CMD"] == "GET_VERSION") {
        resp["status"] = "success";
        resp["version"] = ACHEM_VERSION;

        response = resp.dump();
    } else {
        resp["status"] = "fail";
        resp["message"] = "Invalid command!";

        response = resp.dump();
    }
}

void Coordinator::updateChannels() {
    while (running) {
        configChannels();
        const int rate_ms =
            std::max(1, chem::RuntimeConfig::channel_update_rate_ms.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(rate_ms));
    }
}

// TODO: Add poll rate
void Coordinator::configChannels() {
    for (auto& inm : m_intermediateMap) {
        auto& channels = inm.second->getChannelList();

        for (auto& pair : channels) {
            auto& ch = pair.second;
            const std::string node_1 = ch.getSrc();
            const std::string node_2 = ch.getDest();

            auto findLoc = [&](const std::string& key)
                -> std::optional<chem::NodeLocation> {
                auto nodeIt = m_nodeMap.find(key);
                if (nodeIt != m_nodeMap.end()) {
                    if (auto loc = nodeIt->second.getLocation();
                        loc.has_value())
                        return loc;
                }
                // Fallback: match by name across nodes
                for (const auto& pair : m_nodeMap) {
                    if (pair.second.getConfig().getName() == key) {
                        if (auto loc = pair.second.getLocation();
                            loc.has_value())
                            return loc;
                    }
                }
                return std::nullopt;
            };

            auto loc1Opt = findLoc(node_1);
            auto loc2Opt = findLoc(node_2);

            if (!loc1Opt.has_value() || !loc2Opt.has_value()) continue;

            auto loc1 = loc1Opt->position;
            auto lat1 = loc1.lat;
            auto lon1 = loc1.lon;
            auto alt1 = loc1.alt;

            auto loc2 = loc2Opt->position;
            auto lat2 = loc2.lat;
            auto lon2 = loc2.lon;
            auto alt2 = loc2.alt;

            double dist = calcDistance(lat1, lon1, lat2, lon2);
            double azimuth = calcAzimuth(lat1, lon1, lat2, lon2);
            double d_alt =
                alt2 -
                alt1;  // Signed altitude difference (positive = dest is higher)
            // Elevation angle: positive when looking up, negative when looking
            // down
            double elev = (dist > 0.01) ? atan(d_alt / dist) * 180 / PI : 0.0;

            ch.updateDistance(dist);
            ch.updateAltitude(std::abs(d_alt));
            ch.updateHeights(static_cast<float>(alt1),
                             static_cast<float>(alt2));
            ch.updateElevation(elev);
            ch.updateAzimuth(azimuth);

            if (ch.isDopplerEnabled()) {
                constexpr double meters_per_deg = 111319.5;
                const double lat_avg_rad =
                    ((lat1 + lat2) * 0.5) * (static_cast<double>(PI) / 180.0);
                const double north_m = (lat2 - lat1) * meters_per_deg;
                const double east_m =
                    (lon2 - lon1) * meters_per_deg * std::cos(lat_avg_rad);
                const double up_m = static_cast<double>(alt2 - alt1);
                const double range_m = std::sqrt(
                    east_m * east_m + north_m * north_m + up_m * up_m);
                if (range_m > 0.01) {
                    const double ux = east_m / range_m;
                    const double uy = north_m / range_m;
                    const double uz = up_m / range_m;

                    const auto v1 = loc1Opt->velocity;
                    const auto v2 = loc2Opt->velocity;
                    const double vx_rel =
                        static_cast<double>(v2.east - v1.east);
                    const double vy_rel =
                        static_cast<double>(v2.north - v1.north);
                    const double vz_rel = static_cast<double>(v2.up - v1.up);
                    const double range_rate_mps =
                        vx_rel * ux + vy_rel * uy + vz_rel * uz;

                    const double carrier_hz = inm.first;
                    const double doppler_hz =
                        -(range_rate_mps /
                          static_cast<double>(SPEED_OF_LIGHT)) *
                        carrier_hz;
                    ch.setDopplerHz(doppler_hz);
                } else {
                    ch.setDopplerHz(0.0);
                }
            }
        }
    }
}

double Coordinator::calcAzimuth(const double& lat1, const double& lon1,
                                const double& lat2, const double& lon2) {
    // Convert degrees to radians for trig functions
    constexpr double deg_to_rad = PI / 180.0;
    const double lat1_rad = lat1 * deg_to_rad;
    const double lat2_rad = lat2 * deg_to_rad;
    const double dlon_rad = (lon2 - lon1) * deg_to_rad;

    double azimuth = atan2(sin(dlon_rad) * cos(lat2_rad),
                           (cos(lat1_rad) * sin(lat2_rad) -
                            sin(lat1_rad) * cos(lat2_rad) * cos(dlon_rad)));

    return azimuth * 180 / PI;
}

double Coordinator::calcDistance(const double& lat1, const double& lon1,
                                 const double& lat2, const double& lon2) {
    if (!lat1 || !lat1 || !lon1 || !lon2) return 0;

    constexpr double meters_per_deg = 111319.5;
    constexpr double deg_to_rad = PI / 180.0;

    const double dlat = lat2 - lat1;
    const double dlon = lon2 - lon1;

    // Account for longitude convergence at different latitudes
    const double lat_avg_rad = ((lat1 + lat2) * 0.5) * deg_to_rad;
    const double north_m = dlat * meters_per_deg;
    const double east_m = dlon * meters_per_deg * cos(lat_avg_rad);

    double dist = 0;
    try {
        dist = sqrt(north_m * north_m + east_m * east_m);
    } catch (std::exception& e) {
        LOG_WARN("COORDINATOR", "Couldn't calculate the distance!");
    }

    return dist;
}

std::string Coordinator::getChannels() {
    auto* db = chem::DBHandler::GetInstance("");
    json resp = db->GetChannels();

    return resp.dump();
}

std::string Coordinator::getIndividualChannels() {
    json j;

    for (auto& inm : m_intermediateMap) {
        auto fr_str = std::to_string(inm.first / 1000000);
        auto channels = inm.second->getChannelList();
        const std::string pathLossStr =
            PropagationModelToString(inm.second->getPathLossMode());

        if (!j.contains(fr_str)) {
            j[fr_str] = json::array();
        }

        for (auto& ch : channels) {
            const auto& channel = ch.second;
            const auto noiseType = channel.getNoiseType();
            const std::string noiseModel =
                (noiseType == chem::NoiseType::AWGN) ? "AWGN" : "NONE";

            // Resolve friendly names for source/dest (fallback to IDs)
            auto resolveName = [&](const std::string& id) -> std::string {
                auto it = m_nodeMap.find(id);
                if (it != m_nodeMap.end()) {
                    const auto& n = it->second.getConfig().getName();
                    if (!n.empty()) return n;
                }
                for (const auto& pair : m_nodeMap) {
                    if (pair.second.getConfig().getName() == id) {
                        return pair.second.getConfig().getName();
                    }
                }
                return id;
            };

            const std::string srcName = resolveName(channel.getSrc());
            const std::string destName = resolveName(channel.getDest());

            if (srcName == destName) continue;  // Skip self-channels

            auto l_ch = channel.getIndChannels();
            for (auto& l : l_ch) {
                json entry;
                entry["src"] = srcName;
                entry["srcId"] = channel.getSrc();
                entry["dest"] = destName;
                entry["destId"] = channel.getDest();
                entry["p_src"] = l.first.src;
                entry["p_dest"] = l.first.dest;
                entry["coeff"] = l.second;
                entry["noiseModel"] = noiseModel;
                entry["snr"] = channel.getSNR();
                entry["snrMeasuredDb"] = channel.getMeasuredSNRdB();
                if (channel.hasSNROverride()) {
                    entry["snrOverrideDb"] = channel.getSNR();
                } else {
                    entry["snrOverrideDb"] = nullptr;
                }
                entry["distance"] = channel.getLinkDistance();
                entry["altitude"] = channel.getLinkAltitude();
                entry["elevation"] = channel.getLinkElevation();
                entry["azimuth"] = channel.getLinkAzimuth();
                entry["plDb"] = channel.getPlDb();
                entry["freqOffsetHz"] = channel.getFrequencyOffsetHz();
                entry["dopplerEnabled"] = channel.isDopplerEnabled();
                entry["dopplerHz"] = channel.getDopplerHz();
                entry["pathLoss"] = pathLossStr;
                entry["cirTapCount"] = channel.getChTapCount(l.first);
                entry["extensionActive"] = channel.isExtensionActive();
                entry["activeExtension"] = channel.getActiveExtension();
                j[fr_str].push_back(std::move(entry));
            }
        }
    }
    return j.dump();
}

std::string Coordinator::getNodes() {
    auto* db = chem::DBHandler::GetInstance("");
    json resp = db->GetNodes();

    for (const auto& [id, node] : m_nodeMap) {
        const auto& cfg = node.getConfig();
        const std::string& name = cfg.getName();
        if (name.empty()) continue;
        if (!resp.contains(name) || !resp[name].is_object()) continue;

        // Report the node-level gains that CHEM uses in `Intermediate`
        // (populated from app updates).
        const double tx_gain_raw =
            cfg.hasDeviceTxGain() ? cfg.getDeviceTxGain() : 0.0;
        const double rx_gain_raw =
            cfg.hasDeviceRxGain() ? cfg.getDeviceRxGain() : 0.0;
        const double tx_gain_db =
            (std::isfinite(tx_gain_raw) && std::abs(tx_gain_raw) > 1e-12)
                ? tx_gain_raw
                : TX_GAIN;
        const double rx_gain_db =
            (std::isfinite(rx_gain_raw) && std::abs(rx_gain_raw) > 1e-12)
                ? rx_gain_raw
                : RX_GAIN;
        resp[name]["txGainDb"] = tx_gain_db;
        resp[name]["rxGainDb"] = rx_gain_db;

        // Priority: dynamically set patterns > characteristics > database
        // default
        if (cfg.hasTxAntennaPattern()) {
            resp[name]["txAntennaPattern"] = cfg.getTxAntennaPattern();
        } else if (cfg.hasCharacteristics()) {
            const auto& chars = cfg.getCharacteristics();
            if (chars.has_value() && !chars->tx_antennas.empty()) {
                resp[name]["txAntennaPattern"] = chars->tx_antennas.front();
            }
        }
        if (cfg.hasRxAntennaPattern()) {
            resp[name]["rxAntennaPattern"] = cfg.getRxAntennaPattern();
        } else if (cfg.hasCharacteristics()) {
            const auto& chars = cfg.getCharacteristics();
            if (chars.has_value() && !chars->rx_antennas.empty()) {
                resp[name]["rxAntennaPattern"] = chars->rx_antennas.front();
            }
        }

        if (cfg.hasCharacteristics()) {
            const auto& chars = cfg.getCharacteristics();
            if (chars.has_value() && std::isfinite(chars->source_power_dbfs)) {
                resp[name]["sourcePowerDbfs"] = chars->source_power_dbfs;
            }
        }

        resp[name]["nodeType"] = NodeTypeToString(cfg.getNodeType());
    }

    return resp.dump();
}

static long long _read_proc_self_cpu_ticks() {
    // /proc/self/stat: fields 14 and 15 are utime and stime in clock ticks.
    try {
        std::ifstream f("/proc/self/stat");
        if (!f.is_open()) return -1;

        std::string line;
        std::getline(f, line);
        if (line.empty()) return -1;

        const auto rparen = line.rfind(')');
        if (rparen == std::string::npos) return -1;

        std::istringstream iss(line.substr(rparen + 2));
        std::string token;
        long long utime = 0;
        long long stime = 0;
        for (int idx = 1; iss >> token; ++idx) {
            if (idx == 12) {
                utime = std::stoll(token);
            } else if (idx == 13) {
                stime = std::stoll(token);
                break;
            }
        }
        return utime + stime;
    } catch (...) {
        return -1;
    }
}

static std::optional<long long> _read_status_kb_field(const std::string& key) {
    try {
        std::ifstream f("/proc/self/status");
        if (!f.is_open()) return std::nullopt;
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind(key, 0) != 0) continue;
            std::istringstream iss(line);
            std::string label;
            long long value_kb = 0;
            std::string unit;
            iss >> label >> value_kb >> unit;
            if (iss.fail()) return std::nullopt;
            return value_kb;
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::string Coordinator::getStatus() {
    json resp;
    resp["status"] = "success";
    resp["nodesConnected"] = static_cast<int>(m_nodeMap.size());
    resp["freqChannels"] = static_cast<int>(m_intermediateMap.size());
    resp["version"] = ACHEM_VERSION;
    resp["vehiclePollRateMs"] =
        chem::RuntimeConfig::vehicle_poll_rate_ms.load();
    resp["channelUpdateRateMs"] =
        chem::RuntimeConfig::channel_update_rate_ms.load();

    const auto uptime =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::steady_clock::now() - g_proc_start);
    resp["uptimeSec"] = uptime.count();

    const long long ticks = _read_proc_self_cpu_ticks();
    resp["cpuTicks"] = ticks;
    const long clk_tck = sysconf(_SC_CLK_TCK);
    resp["clkTck"] = clk_tck;
    if (ticks >= 0 && clk_tck > 0) {
        const double cpu_time_sec =
            static_cast<double>(ticks) / static_cast<double>(clk_tck);
        resp["cpuTimeSec"] = cpu_time_sec;
        const unsigned int cores =
            std::max(1u, std::thread::hardware_concurrency());
        resp["cpuCores"] = static_cast<int>(cores);
        const double denom =
            std::max(1e-9, uptime.count() * static_cast<double>(cores));
        resp["cpuPercentAvg"] = (cpu_time_sec / denom) * 100.0;
    }

    if (auto vmrss_kb = _read_status_kb_field("VmRSS:"); vmrss_kb.has_value()) {
        resp["rssBytes"] = (*vmrss_kb) * 1024;
    }
    if (auto vmsize_kb = _read_status_kb_field("VmSize:");
        vmsize_kb.has_value()) {
        resp["vmSizeBytes"] = (*vmsize_kb) * 1024;
    }

    return resp.dump();
}

std::string Coordinator::getAntennaPatterns() {
    json resp = json::array();

    try {
        namespace fs = std::filesystem;
        fs::path root = fs::path("data") / "antenna";
        if (!fs::exists(root) || !fs::is_directory(root)) {
            return resp.dump();
        }

        for (const auto& entry : fs::directory_iterator(root)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            resp.push_back(name);
        }
    } catch (...) {
    }

    return resp.dump();
}

std::string Coordinator::getAntennaTypes() {
    json resp = json::array();
    try {
        for (const auto& name : chem::antennas::AvailableAntennas()) {
            resp.push_back(name);
        }
    } catch (...) {
        LOG_WARN("COORDINATOR", "Couldn't get antenna types!");
    }
    return resp.dump();
}

void Coordinator::setShadowingSTD() {
    // Iterate through all the channels
    // Update the shadowing Std
    for (const auto& [key, it] : m_intermediateMap) {
        it->setShadowingSTD(shadow_std);
    }
}

std::string Coordinator::setPathLossMode(const double& freq,
                                         const std::string plMode) {
    return setPathLossMode(freq, plMode, -1.0f);
}

std::string Coordinator::setPathLossMode(const double& freq,
                                         const std::string plMode,
                                         float grCoeff) {
    return setPathLossMode(freq, plMode, grCoeff, "UMa");
}

std::string Coordinator::setPathLossMode(const double& freq,
                                         const std::string plMode,
                                         float grCoeff,
                                         const std::string& scenario) {
    auto it = m_intermediateMap.find(freq);
    json j;

    if (it == m_intermediateMap.end()) {
        j["result"] = false;
        return j.dump();
    }

    it->second->updatePathLoss(plMode, grCoeff);
    if (PropagationModelFromString(plMode) ==
        PropagationModel::THREE_GPP_38_901) {
        it->second->set3gppScenario(scenario);
    }
    j["result"] = true;

    return j.dump();
}

std::string Coordinator::setChCoeff(const double& freq, const std::string& src,
                                    const std::string& dest,
                                    const struct chId& ch,
                                    const double& coeff) {
    auto it = m_intermediateMap.find(freq);
    json j;

    if (it == m_intermediateMap.end()) {
        j["result"] = false;
        return j.dump();
    }

    const std::string srcId = resolveNodeId(m_nodeMap, src);
    const std::string destId = resolveNodeId(m_nodeMap, dest);
    j["result"] = it->second->updateChannelCoeff(srcId, destId, ch, coeff);

    return j.dump();
}

// TODO: use noisemodel type here
std::string Coordinator::setNoiseModel(const double& freq,
                                       const std::string& src,
                                       const std::string& dest,
                                       const std::string noiseMdl,
                                       const double& snr) {
    auto it = m_intermediateMap.find(freq);
    json j;

    if (it == m_intermediateMap.end()) {
        j["result"] = false;

        return j.dump();
    }

    bool awgn = noiseMdl == "AWGN";

    const std::string srcId = resolveNodeId(m_nodeMap, src);
    const std::string destId = resolveNodeId(m_nodeMap, dest);
    j["result"] = it->second->updateAWGN(srcId, destId, awgn, snr);

    return j.dump();
}

std::string Coordinator::setFrequencyOffset(const double& freq,
                                            const std::string& src,
                                            const std::string& dest,
                                            const double& freq_offset_hz) {
    auto it = m_intermediateMap.find(freq);
    json j;

    if (it == m_intermediateMap.end()) {
        j["result"] = false;
        return j.dump();
    }

    const std::string srcId = resolveNodeId(m_nodeMap, src);
    const std::string destId = resolveNodeId(m_nodeMap, dest);
    j["result"] =
        it->second->updateFrequencyOffset(srcId, destId, freq_offset_hz);
    return j.dump();
}

std::string Coordinator::setDopplerEnabled(const double& freq,
                                           const std::string& src,
                                           const std::string& dest,
                                           const bool& enabled) {
    auto it = m_intermediateMap.find(freq);
    json j;

    if (it == m_intermediateMap.end()) {
        j["result"] = false;
        return j.dump();
    }

    const std::string srcId = resolveNodeId(m_nodeMap, src);
    const std::string destId = resolveNodeId(m_nodeMap, dest);
    j["result"] = it->second->updateDopplerEnabled(srcId, destId, enabled);
    return j.dump();
}

std::string Coordinator::setCIR(const double& freq, const std::string& src,
                                const std::string& dest, const struct chId& ch,
                                const chem::signal_v& taps) {
    auto it = m_intermediateMap.find(freq);
    json j;

    if (it == m_intermediateMap.end()) {
        j["result"] = false;
        return j.dump();
    }

    const std::string srcId = resolveNodeId(m_nodeMap, src);
    const std::string destId = resolveNodeId(m_nodeMap, dest);
    j["result"] = it->second->updateCIR(srcId, destId, ch, taps);
    return j.dump();
}
