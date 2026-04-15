/**
 * @file controller.h
 * @brief CHEM controller for node management
 * @author Anıl Gürses
 * @version v1.0
 */

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/io_service.hpp>
#include <map>
#include <memory>

#include "../channel/intermediate.h"
#include "../common.h"
#include "../models/data_pool.hpp"
#include "../net/tcp_server.h"
#include "../node/node_entity.h"
#include "../vehicle/handler.h"
#include "spdlog/spdlog.h"

namespace chem {
namespace emulator {
class Transmitter;
class Receiver;
typedef std::shared_ptr<Transmitter> transmitterSPtr;
typedef std::shared_ptr<Receiver> receiverSPtr;

typedef std::shared_ptr<chem::Intermediate> intermediateSPtr;

class Controller : public std::enable_shared_from_this<Controller> {
   public:
    Controller() = delete;

    Controller(std::string ipAddr, uint16_t port,
               std::map<std::string, chem::Node>& nodeMap,
               std::map<double, intermediateSPtr>& intermediateMap,
               std::shared_ptr<chem::PropagationDefaults> propagationDefaults =
                   nullptr,
               int numa_node = 0, bool numa_enabled = false);

    ~Controller() = default;

    void Start();

    void requestHandler(const std::string& request, std::string& response);

    void GeneratePortPool(uint16_t portEnd, std::vector<uint16_t>& portPool);

    uint16_t AssignTXPort(const std::string& ipAddress);

    void RestoreTXPort(const std::string& ipAddress);

    uint16_t AssignRXPort(const std::string& ipAddress);

    void AddNode(const chem::Node& node);

    bool FindChannel(const double& freq, intermediateSPtr& channel);

    intermediateSPtr CreateChannel(const double& freq, const double& srate);

    bool AssignULChannel(chem::Node& node, chem::NodeConfig newConfig);

    bool AssignDLChannel(chem::Node& node, chem::NodeConfig newConfig);

    void CheckTimeouts();

   private:
    std::pair<std::map<std::string, chem::Node>::iterator, bool>
    GetOrCreateNode(const chem::Node& node);

   private:
    // TODO: change it to unique ptr
    transmitterSPtr CreateTransmitter(chem::Node& node);

    receiverSPtr CreateReceiver(chem::Node& node);

    void isChannelEmpty(intermediateSPtr& intermediate);

    TCPServer m_tcpServer;

    std::vector<uint16_t> m_rxPortPool;
    std::vector<uint16_t> m_txPortPool;

    std::string m_ipAddr;
    uint16_t m_port;

    std::map<std::string, chem::Node>& m_nodeMap;
    // Ip Address and number of nodes in that ip
    std::map<std::string, uint8_t> m_ipAddrMap;

    payloadPoolSPtr payload_pool;
    iqPoolSPtr iq_pool;  // convenience alias to m_iq_pools[m_numa_node]
    std::vector<iqPoolSPtr> m_iq_pools;
    int m_numa_node = 0;
    bool m_numa_enabled = false;

    boost::asio::io_service io_service;

    std::map<double, intermediateSPtr>& m_intermediateMap;
    std::shared_ptr<chem::PropagationDefaults> m_propagationDefaults;
    std::map<std::string, std::shared_ptr<chem::vehicle::Handler>>
        m_vehicleHandlers;

    uint16_t m_numNodes = 0;
    uint16_t m_numChannels = 0;
};
}  // namespace emulator
}  // namespace chem
