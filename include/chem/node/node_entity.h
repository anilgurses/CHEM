/**
 * @file node_entity.h
 * @brief Node entity header file
 * @author Anıl Gürses
 * @version v1.0
 */

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "../aerpaw/node_characteristics.h"
#include "../common.h"
#include "../models/node_config.hpp"
#include "../nlohmann/json.hpp"

namespace chem::emulator {
class Transmitter;
class Receiver;
using transmitterSPtr = std::shared_ptr<Transmitter>;
using receiverSPtr = std::shared_ptr<Receiver>;
}  // namespace chem::emulator

namespace chem {
namespace vehicle {
class Handler;
}  // namespace vehicle

class Node {
   public:
    Node();
    explicit Node(NodeConfig config);
    Node(const Node& other);
    Node& operator=(const Node& other);
    Node(Node&& other) noexcept;
    Node& operator=(Node&& other) noexcept;
    ~Node() = default;

    void updateHeartbeat();

    bool isAlive(int timeout = 10) const;

    friend bool operator==(const Node& n1, const Node& n2);

    friend bool operator!=(const Node& n1, const Node& n2);

    bool compare(const Node& n);

    bool compare(const NodeConfig& nc);

    void setNodeType(NodeType type);

    NodeType getNodeType() const;

    void setLocation(const chem::Location& location, uint8_t logicalNode = 0);

    void setVelocity(const chem::Velocity& velocity);

    bool hasLocation() const;

    std::optional<chem::NodeLocation> getLocation() const;

    bool isComplete() const;

    bool isTxExist() const;

    bool isRxExist() const;

    void setUpdate(bool u);

    bool isUpdate() const;

    void setTransmitter(emulator::transmitterSPtr tx);

    emulator::transmitterSPtr getTransmitter();

    void setReceiver(emulator::receiverSPtr rx);

    emulator::receiverSPtr getReceiver();

    const NodeConfig& getConfig() const;

    NodeConfig& getConfig();

    void setConfig(NodeConfig config);

    void printConfig() const;

    void refreshFromConfig(
        NodeConfig& nodeConfig,
        std::map<std::string, std::shared_ptr<chem::vehicle::Handler>>&
            vehicleHandlers);

    bool addVehicle(
        const NodeConfig& nodeConfig,
        std::map<std::string, std::shared_ptr<chem::vehicle::Handler>>&
            vehicleHandlers);
    void setCharacteristics(
        const chem::aerpaw::NodeCharacteristics& characteristics);
    void clearCharacteristics();
    bool hasCharacteristics() const;
    const std::optional<chem::aerpaw::NodeCharacteristics>& getCharacteristics()
        const;

   private:
    NodeConfig m_config;
    bool m_update{false};

    bool m_txExist = false;
    bool m_rxExist = false;

    NodeType m_type{NodeType::UNKNOWN};
    chem::Location m_location{};
    uint8_t m_logicalNode = 0;
    bool m_hasLocation = false;

    mutable std::mutex m_locMutex;
    chem::Velocity m_velocity{};
    bool m_hasVelocity{false};
    bool m_hasPrevLocation{false};
    chem::Location m_prevLocation{};
    std::chrono::steady_clock::time_point m_prevLocationTime{};
    std::optional<chem::aerpaw::NodeCharacteristics> m_characteristics;

    emulator::transmitterSPtr m_transmitter;
    emulator::receiverSPtr m_receiver;
    std::chrono::steady_clock::time_point m_lastHeartbeat;
};

void to_json(json& j, const Node& n);

void from_json(const json& j, Node& n);

}  // namespace chem
