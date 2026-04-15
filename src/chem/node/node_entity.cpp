/**
 * @file node_entity.cpp
 * @brief Node entity cpp file
 * @author Anıl Gürses
 * @version v1.0
 */
#include "chem/node/node_entity.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <thread>
#include <utility>

#include "chem/aerpaw/fixed_node.h"
#include "chem/aerpaw/portable_node.h"
#include "chem/vehicle/handler.h"

using namespace chem;

Node::Node() { m_lastHeartbeat = std::chrono::steady_clock::now(); }

Node::Node(NodeConfig config) : m_config(std::move(config)) {
    m_lastHeartbeat = std::chrono::steady_clock::now();
}

Node::Node(const Node& other) {
    std::lock_guard<std::mutex> lock(other.m_locMutex);
    m_config = other.m_config;
    m_update = other.m_update;
    m_txExist = other.m_txExist;
    m_rxExist = other.m_rxExist;
    m_type = other.m_type;
    m_location = other.m_location;
    m_logicalNode = other.m_logicalNode;
    m_hasLocation = other.m_hasLocation;
    m_velocity = other.m_velocity;
    m_hasVelocity = other.m_hasVelocity;
    m_hasPrevLocation = other.m_hasPrevLocation;
    m_prevLocation = other.m_prevLocation;
    m_prevLocationTime = other.m_prevLocationTime;
    m_characteristics = other.m_characteristics;
    m_transmitter = other.m_transmitter;
    m_receiver = other.m_receiver;
    m_lastHeartbeat = other.m_lastHeartbeat;
}

Node& Node::operator=(const Node& other) {
    if (this == &other) return *this;
    std::scoped_lock lock(m_locMutex, other.m_locMutex);
    m_config = other.m_config;
    m_update = other.m_update;
    m_txExist = other.m_txExist;
    m_rxExist = other.m_rxExist;
    m_type = other.m_type;
    m_location = other.m_location;
    m_logicalNode = other.m_logicalNode;
    m_hasLocation = other.m_hasLocation;
    m_velocity = other.m_velocity;
    m_hasVelocity = other.m_hasVelocity;
    m_hasPrevLocation = other.m_hasPrevLocation;
    m_prevLocation = other.m_prevLocation;
    m_prevLocationTime = other.m_prevLocationTime;
    m_characteristics = other.m_characteristics;
    m_transmitter = other.m_transmitter;
    m_receiver = other.m_receiver;
    m_lastHeartbeat = other.m_lastHeartbeat;
    return *this;
}

Node::Node(Node&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.m_locMutex);
    m_config = std::move(other.m_config);
    m_update = other.m_update;
    m_txExist = other.m_txExist;
    m_rxExist = other.m_rxExist;
    m_type = other.m_type;
    m_location = other.m_location;
    m_logicalNode = other.m_logicalNode;
    m_hasLocation = other.m_hasLocation;
    m_velocity = other.m_velocity;
    m_hasVelocity = other.m_hasVelocity;
    m_hasPrevLocation = other.m_hasPrevLocation;
    m_prevLocation = other.m_prevLocation;
    m_prevLocationTime = other.m_prevLocationTime;
    m_characteristics = std::move(other.m_characteristics);
    m_transmitter = std::move(other.m_transmitter);
    m_receiver = std::move(other.m_receiver);
    m_lastHeartbeat = other.m_lastHeartbeat;
}

Node& Node::operator=(Node&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(m_locMutex, other.m_locMutex);
    m_config = std::move(other.m_config);
    m_update = other.m_update;
    m_txExist = other.m_txExist;
    m_rxExist = other.m_rxExist;
    m_type = other.m_type;
    m_location = other.m_location;
    m_logicalNode = other.m_logicalNode;
    m_hasLocation = other.m_hasLocation;
    m_velocity = other.m_velocity;
    m_hasVelocity = other.m_hasVelocity;
    m_hasPrevLocation = other.m_hasPrevLocation;
    m_prevLocation = other.m_prevLocation;
    m_prevLocationTime = other.m_prevLocationTime;
    m_characteristics = std::move(other.m_characteristics);
    m_transmitter = std::move(other.m_transmitter);
    m_receiver = std::move(other.m_receiver);
    m_lastHeartbeat = other.m_lastHeartbeat;
    return *this;
}

void Node::updateHeartbeat() {
    m_lastHeartbeat = std::chrono::steady_clock::now();
}

bool Node::isAlive(int timeout) const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - m_lastHeartbeat)
               .count() < timeout;
}

bool chem::operator==(const Node& n1, const Node& n2) {
    return (n1.getConfig().getId() == n2.getConfig().getId());
}

bool chem::operator!=(const Node& n1, const Node& n2) { return !(n1 == n2); }

bool Node::compare(const Node& n) {
    bool result = true;
    result &= (m_config.getId() == n.getConfig().getId());
    result &= (m_config.getName() == n.getConfig().getName());
    result &= (m_config.getIpAddress() == n.getConfig().getIpAddress());
    result &= (m_config.getSampleRate() == n.getConfig().getSampleRate());
    result &= (m_config.getInputFormat() == n.getConfig().getInputFormat());
    result &= (m_config.getOutputFormat() == n.getConfig().getOutputFormat());
    result &=
        (m_config.getChannels().size() == n.getConfig().getChannels().size());
    result &= (m_config.getNodeType() == n.getConfig().getNodeType());
    result &= (m_config.hasDeviceTxGain() == n.getConfig().hasDeviceTxGain());
    result &= (m_config.hasDeviceRxGain() == n.getConfig().hasDeviceRxGain());
    if (m_config.hasDeviceTxGain() && n.getConfig().hasDeviceTxGain()) {
        result &=
            (m_config.getDeviceTxGain() == n.getConfig().getDeviceTxGain());
    }
    if (m_config.hasDeviceRxGain() && n.getConfig().hasDeviceRxGain()) {
        result &=
            (m_config.getDeviceRxGain() == n.getConfig().getDeviceRxGain());
    }
    const auto& ch1 = m_config.getChannels();
    const auto& ch2 = n.getConfig().getChannels();
    if (ch1.size() == ch2.size()) {
        for (size_t i = 0; i < ch1.size(); i++) {
            result &= ch1[i].compare(ch2[i]);
        }
    } else {
        return false;
    }
    return result;
}

bool Node::compare(const NodeConfig& nc) {
    bool result = true;
    result &= (m_config.getId() == nc.getId());
    result &= (m_config.getName() == nc.getName());
    result &= (m_config.getIpAddress() == nc.getIpAddress());
    result &= (m_config.getSampleRate() == nc.getSampleRate());
    result &= (m_config.getInputFormat() == nc.getInputFormat());
    result &= (m_config.getOutputFormat() == nc.getOutputFormat());
    result &= (m_config.getChannels().size() == nc.getChannels().size());
    result &= (m_config.getNodeType() == nc.getNodeType());
    result &= (m_config.hasDeviceTxGain() == nc.hasDeviceTxGain());
    result &= (m_config.hasDeviceRxGain() == nc.hasDeviceRxGain());
    if (m_config.hasDeviceTxGain() && nc.hasDeviceTxGain()) {
        result &= (m_config.getDeviceTxGain() == nc.getDeviceTxGain());
    }
    if (m_config.hasDeviceRxGain() && nc.hasDeviceRxGain()) {
        result &= (m_config.getDeviceRxGain() == nc.getDeviceRxGain());
    }
    const auto& ch1 = m_config.getChannels();
    const auto& ch2 = nc.getChannels();
    if (ch1.size() == ch2.size()) {
        for (size_t i = 0; i < ch1.size(); i++) {
            result &= ch1[i].compare(ch2[i]);
        }
    } else {
        return false;
    }
    return result;
}

void Node::setNodeType(NodeType type) { m_type = type; }

NodeType Node::getNodeType() const { return m_type; }

void Node::setLocation(const chem::Location& location, uint8_t logicalNode) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_locMutex);

    if (m_hasPrevLocation) {
        const auto dt =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                now - m_prevLocationTime)
                .count();
        if (dt > 0.001) {
            constexpr double meters_per_deg = 111319.5;
            const double lat_avg_rad =
                ((m_prevLocation.lat + location.lat) * 0.5) *
                (static_cast<double>(PI) / 180.0);
            const double dlat_m =
                (location.lat - m_prevLocation.lat) * meters_per_deg;
            const double dlon_m = (location.lon - m_prevLocation.lon) *
                                  meters_per_deg * std::cos(lat_avg_rad);
            const double dalt_m =
                static_cast<double>(location.alt - m_prevLocation.alt);

            m_velocity.north = static_cast<float>(dlat_m / dt);
            m_velocity.east = static_cast<float>(dlon_m / dt);
            m_velocity.up = static_cast<float>(dalt_m / dt);
            m_hasVelocity = true;
        }
    }

    m_prevLocation = location;
    m_prevLocationTime = now;
    m_hasPrevLocation = true;

    m_location = location;
    m_logicalNode = logicalNode;
    m_hasLocation = true;
    m_config.setLocation(location);
    m_config.setLogicalNode(logicalNode);
}

void Node::setVelocity(const chem::Velocity& velocity) {
    std::lock_guard<std::mutex> lock(m_locMutex);
    m_velocity = velocity;
    m_hasVelocity = true;
}

bool Node::hasLocation() const { return m_hasLocation; }

std::optional<chem::NodeLocation> Node::getLocation() const {
    std::lock_guard<std::mutex> lock(m_locMutex);
    if (!m_hasLocation) return std::nullopt;
    chem::NodeLocation out;
    out.logicalNode = m_logicalNode;
    out.position = m_location;
    out.velocity = m_velocity;
    out.hasVelocity = m_hasVelocity;
    return out;
}

bool Node::isComplete() const {
    if (m_config.getChannels().size() < 1) return false;
    return true;
}

bool Node::isTxExist() const { return m_txExist; }

bool Node::isRxExist() const { return m_rxExist; }

void Node::setUpdate(bool u) { m_update = u; }

bool Node::isUpdate() const { return m_update; }

void Node::setTransmitter(emulator::transmitterSPtr tx) {
    m_transmitter = tx;
    if (tx != nullptr)
        m_txExist = true;
    else
        m_txExist = false;
}

emulator::transmitterSPtr Node::getTransmitter() { return m_transmitter; }

void Node::setReceiver(emulator::receiverSPtr rx) {
    m_receiver = rx;
    if (rx != nullptr)
        m_rxExist = true;
    else
        m_rxExist = false;
}

emulator::receiverSPtr Node::getReceiver() { return m_receiver; }

const NodeConfig& Node::getConfig() const { return m_config; }

NodeConfig& Node::getConfig() { return m_config; }

void Node::setConfig(NodeConfig config) {
    m_config = std::move(config);
    if (m_config.hasCharacteristics()) {
        m_characteristics = m_config.getCharacteristics();
    } else {
        m_characteristics.reset();
    }
}

void Node::setCharacteristics(
    const chem::aerpaw::NodeCharacteristics& characteristics) {
    m_characteristics = characteristics;
}

void Node::clearCharacteristics() { m_characteristics.reset(); }

bool Node::hasCharacteristics() const { return m_characteristics.has_value(); }

const std::optional<chem::aerpaw::NodeCharacteristics>&
Node::getCharacteristics() const {
    return m_characteristics;
}

void Node::printConfig() const {
    LOG_INFO("NODE", fmt::format("Node ID: {}", m_config.getId()));
    LOG_INFO("NODE", fmt::format("Node Name: {}", m_config.getName()));
    LOG_INFO("NODE",
             fmt::format("Node IP Address: {}", m_config.getIpAddress()));
    LOG_INFO("NODE",
             fmt::format("Node TX Port: {}", m_config.getTxPortNumber()));
    LOG_INFO("NODE",
             fmt::format("Node RX Port: {}", m_config.getRxPortNumber()));
    LOG_INFO("NODE", fmt::format("Node RX Sample Rate: {} Sps",
                                 m_config.getSampleRate().getRxRate()));
    if (m_config.getChannels().empty()) {
        LOG_INFO("NODE", "No channels assigned!");
        return;
    }
    for (const auto& ch : m_config.getChannels()) {
        LOG_INFO("NODE", fmt::format("Channel Number: {}", 1));
        LOG_INFO("NODE",
                 fmt::format("Channel TX Freq: {} MHz", ch.getTxFreq() / 1e6));
        LOG_INFO("NODE",
                 fmt::format("Channel RX Freq: {} MHz", ch.getRxFreq() / 1e6));
    }
}

void chem::to_json(json& j, const Node& n) {
    try {
        j = json(n.getConfig());
    } catch (std::exception& e) {
        LOG_DEBUG("NODE", "Couldn't convert to json!");
    }
}

void chem::from_json(const json& j, Node& n) {
    try {
        NodeConfig config = j.get<NodeConfig>();
        n.setConfig(config);
        n.setUpdate(j.at("update").get<bool>());
    } catch (std::exception& e) {
        LOG_DEBUG("NODE",
                  fmt::format("Couldn't parse the json! Cause: {}", e.what()));
    }
}

bool Node::addVehicle(
    const NodeConfig& nodeConfig,
    std::map<std::string, std::shared_ptr<chem::vehicle::Handler>>&
        vehicleHandlers) {
    auto key =
        !m_config.getName().empty() ? m_config.getName() : m_config.getId();
    if (key.empty()) return false;
    if (vehicleHandlers.find(key) != vehicleHandlers.end()) return false;

    std::string vehicleAddress = nodeConfig.getVehicleAddress();

    if (vehicleAddress.empty() && nodeConfig.hasAerpawKey()) {
        std::string upperKey = nodeConfig.getAerpawKey();
        std::transform(
            upperKey.begin(), upperKey.end(), upperKey.begin(),
            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upperKey.rfind("PN", 0) == 0) {
            int logicalNode = 0;
            if (nodeConfig.hasLogicalNode() &&
                nodeConfig.getLogicalNode() >= 0) {
                logicalNode = nodeConfig.getLogicalNode();
            } else {
                auto num_pos = upperKey.find_first_of("0123456789");
                if (num_pos != std::string::npos) {
                    try {
                        logicalNode = std::stoi(upperKey.substr(num_pos));
                    } catch (...) {
                        logicalNode = 0;
                    }
                }
            }
            vehicleAddress =
                fmt::format("tcp://127.0.0.1:{}", 5760 + logicalNode);
        }
    }

    if (vehicleAddress.empty()) return false;

    m_config.setVehicleAddress(vehicleAddress);

    auto handler =
        std::make_shared<chem::vehicle::Handler>(vehicleAddress, key, *this);
    std::thread vhcl_th(&chem::vehicle::Handler::Start, handler);
    vhcl_th.detach();
    vehicleHandlers.emplace(key, std::move(handler));
    LOG_INFO("CONTROLLER",
             fmt::format("Started vehicle handler for node \"{}\" ({})", key,
                         vehicleAddress));
    return true;
}

void Node::refreshFromConfig(
    NodeConfig& nodeConfig,
    std::map<std::string, std::shared_ptr<chem::vehicle::Handler>>&
        vehicleHandlers) {
    m_config.setId(nodeConfig.getId());
    m_config.setName(nodeConfig.getName());
    m_config.setIpAddress(nodeConfig.getIpAddress());
    m_config.setInputFormat(nodeConfig.getInputFormat());
    m_config.setOutputFormat(nodeConfig.getOutputFormat());
    m_config.setNumChannels(nodeConfig.getNumChannels());
    m_config.setNodeType(nodeConfig.getNodeType());
    if (nodeConfig.hasVehicleAddress()) {
        m_config.setVehicleAddress(nodeConfig.getVehicleAddress());
    }
    if (nodeConfig.hasAerpawKey()) {
        m_config.setAerpawKey(nodeConfig.getAerpawKey());
    }
    if (nodeConfig.hasDeviceTxGain()) {
        m_config.setDeviceTxGain(nodeConfig.getDeviceTxGain());
    }
    if (nodeConfig.hasDeviceRxGain()) {
        m_config.setDeviceRxGain(nodeConfig.getDeviceRxGain());
    }

    auto nodeType = nodeConfig.getNodeType();
    bool vehicleAdded = false;
    bool isPnNode = false;

    auto resolveLogicalNode = [&nodeConfig]() -> int {
        if (nodeConfig.hasLogicalNode() && nodeConfig.getLogicalNode() >= 0) {
            return nodeConfig.getLogicalNode();
        }

        auto key = nodeConfig.getAerpawKey();
        auto num_pos = key.find_first_of("0123456789");
        if (num_pos != std::string::npos) {
            try {
                auto logicalNode = std::stoi(key.substr(num_pos));
                nodeConfig.setLogicalNode(logicalNode);
                return logicalNode;
            } catch (...) {
            }
        }

        if (!nodeConfig.hasLogicalNode() || nodeConfig.getLogicalNode() < 0) {
            nodeConfig.setLogicalNode(0);
        }

        return nodeConfig.getLogicalNode();
    };
    std::optional<std::pair<int, bool>> resolvedLogicalNode;
    auto getResolvedLogicalNode = [&]() -> std::pair<int, bool> {
        if (resolvedLogicalNode.has_value()) return *resolvedLogicalNode;
        bool hasInfo =
            nodeConfig.hasLogicalNode() && nodeConfig.getLogicalNode() >= 0;

        auto logicalNode = resolveLogicalNode();

        if (!hasInfo) {
            auto key = nodeConfig.getAerpawKey();
            auto num_pos = key.find_first_of("0123456789");
            hasInfo = num_pos != std::string::npos;
        }
        resolvedLogicalNode = std::make_pair(logicalNode, hasInfo);
        return *resolvedLogicalNode;
    };

    if (nodeConfig.hasAerpawKey()) {
        std::string upperKey = nodeConfig.getAerpawKey();
        std::transform(
            upperKey.begin(), upperKey.end(), upperKey.begin(),
            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        isPnNode = upperKey.rfind("PN", 0) == 0;
    }

    if (nodeConfig.hasVehicleAddress()) {
        nodeType = chem::NodeType::VEHICLE_MAVPROXY;
        vehicleAdded = addVehicle(nodeConfig, vehicleHandlers);
    } else if (nodeConfig.hasLocation()) {
        auto [lat, lon, alt] = nodeConfig.getLocation();
        if (!(lat == 0.0 && lon == 0.0 && alt == 0.0f)) {
            nodeType = chem::NodeType::FIXED;
        }
    } else if (nodeConfig.hasAerpawKey()) {
        if (isPnNode) {
            nodeType = chem::NodeType::AERPAW_PORTABLE;
            auto [logicalNode, hasInfo] = getResolvedLogicalNode();
            if (!hasInfo) {
                LOG_WARN("CONTROLLER",
                         fmt::format("Portable node \"{}\" missing "
                                     "logical_node; defaulting to 0.",
                                     nodeConfig.getName()));
            }
        } else {
            nodeType = chem::NodeType::AERPAW_FIXED;
        }
    }
    nodeConfig.setNodeType(nodeType);
    m_config.setNodeType(nodeType);
    if (nodeType == chem::NodeType::AERPAW_FIXED) {
        const auto characteristics =
            chem::aerpaw::fixed_node::characteristics();
        setCharacteristics(characteristics);
        nodeConfig.setCharacteristics(characteristics);
        m_config.setCharacteristics(characteristics);
    } else if (nodeType == chem::NodeType::AERPAW_PORTABLE) {
        const auto characteristics =
            chem::aerpaw::portable_node::characteristics();
        setCharacteristics(characteristics);
        nodeConfig.setCharacteristics(characteristics);
        m_config.setCharacteristics(characteristics);
    } else {
        clearCharacteristics();
        nodeConfig.clearCharacteristics();
        m_config.clearCharacteristics();
    }

    if (nodeConfig.hasLocation()) {
        // TODO: replace the characteristics with something more generic
        auto [lat, lon, alt] = nodeConfig.getLocation();
        if (lat == 0.0 && lon == 0.0 && alt == 0.0f) {
            LOG_WARN("CONTROLLER",
                     fmt::format(
                         "Node \"{}\" reported location as (0,0,0); ignoring.",
                         nodeConfig.getName()));
        } else {
            uint8_t logicalNode = 0;
            if (nodeConfig.hasLogicalNode()) {
                logicalNode = static_cast<uint8_t>(nodeConfig.getLogicalNode());
            }

            chem::Location loc{lat, lon, alt};
            setLocation(loc, logicalNode);
            nodeConfig.setLocation(loc);
            nodeConfig.setLogicalNode(logicalNode);
            LOG_DEBUG(
                "CONTROLLER",
                fmt::format("Updated location for node \"{}\" -> ({}, {}, {})",
                            nodeConfig.getName(), lat, lon, alt));

            const auto characteristics =
                chem::aerpaw::fixed_node::characteristics();
            setCharacteristics(characteristics);
            m_config.setCharacteristics(characteristics);
        }
    } else if (nodeConfig.hasAerpawKey()) {
        auto locOpt = chem::ResolveAerpawLocation(nodeConfig.getAerpawKey());
        if (locOpt.has_value()) {
            uint8_t logicalNode = 0;
            if (nodeConfig.hasLogicalNode()) {
                logicalNode = static_cast<uint8_t>(nodeConfig.getLogicalNode());
            } else {
                logicalNode =
                    static_cast<uint8_t>(getResolvedLogicalNode().first);
            }

            setLocation(*locOpt, logicalNode);
            nodeConfig.setLocation(*locOpt);
            nodeConfig.setLogicalNode(logicalNode);
            LOG_DEBUG(
                "CONTROLLER",
                fmt::format("Resolved AERPAW key \"{}\" for node \"{}\" -> "
                            "({}, {}, {})",
                            nodeConfig.getAerpawKey(), nodeConfig.getName(),
                            locOpt->lat, locOpt->lon, locOpt->alt));
        } else {
            if (!isPnNode) {
                LOG_WARN("CONTROLLER",
                         fmt::format("AERPAW key \"{}\" unknown; location will "
                                     "be updated later.",
                                     nodeConfig.getAerpawKey()));
            }
        }
    }

    if (isPnNode && !vehicleAdded) {
        auto [logicalNode, hasInfo] = getResolvedLogicalNode();
        if (hasInfo) {
            nodeConfig.setVehicleAddress(
                fmt::format("tcp://127.0.0.1:{}", 5760 + logicalNode));
            vehicleAdded = addVehicle(nodeConfig, vehicleHandlers);
        }
    }

    if (nodeConfig.hasVehicleAddress()) {
        m_config.setVehicleAddress(nodeConfig.getVehicleAddress());
    }
}
