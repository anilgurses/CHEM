#include "chem/node/controller.h"

#include <pthread.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

#include "chem/common.h"
#include "chem/db/dbHandler.h"
#include "chem/models/data_pool.hpp"
#include "chem/node/receiver.h"
#include "chem/node/transmitter.h"
#include "chem/numa_utils.h"
#include "chem/runtime_config.h"

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace chem {
namespace emulator {

// ----------------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------------

Controller::Controller(
    std::string ipAddr, uint16_t port, std::map<std::string, Node>& nodeMap,
    std::map<double, intermediateSPtr>& intermediateMap,
    std::shared_ptr<chem::PropagationDefaults> propagationDefaults,
    int numa_node, bool numa_enabled)
    : m_ipAddr(std::move(ipAddr)),
      m_port(port),
      m_nodeMap(nodeMap),
      m_intermediateMap(intermediateMap),
      m_propagationDefaults(std::move(propagationDefaults)),
      m_rxPortPool(400),
      m_txPortPool(400),
      m_numa_node(numa_node),
      m_numa_enabled(numa_enabled) {
    payload_pool = std::make_shared<udpDataPool_t>(POOL_CAP, "UDP Payload");

    if (m_numa_enabled && chem::numa::available()) {
        int n_nodes = chem::numa::node_count();
        m_iq_pools.reserve(n_nodes);
        size_t per_node_cap =
            std::max(size_t(1), static_cast<size_t>(IQ_POOL_CAP) / n_nodes);
        for (int i = 0; i < n_nodes; i++) {
            m_iq_pools.push_back(std::make_shared<iqPool_t>(
                per_node_cap, fmt::format("IQ-node{}", i), i));
        }
        iq_pool = m_iq_pools[m_numa_node];
        LOG_INFO("CONTROLLER",
                 fmt::format("Created {} NUMA IQ pools ({} buffers each)",
                             n_nodes, per_node_cap));
    } else {
        iq_pool = std::make_shared<iqPool_t>(IQ_POOL_CAP, "IQ");
        m_iq_pools.push_back(iq_pool);
    }

    GeneratePortPool(REQ_PORT_NUM(400), m_rxPortPool);
    GeneratePortPool(REP_PORT_NUM(400), m_txPortPool);
}

void Controller::Start() {
    // Start background timeout checker
    std::thread timeout_thread(&Controller::CheckTimeouts, this);
    timeout_thread.detach();

    // Setup TCP server request handler
    m_tcpServer.Bind(
        m_ipAddr, m_port,
        [self = shared_from_this()](const std::string& req, std::string& resp) {
            self->requestHandler(req, resp);
        });

    LOG_INFO("CONTROLLER",
             fmt::format("Node Controller started listening on {}:{}", m_ipAddr,
                         m_port));

    m_tcpServer.startAccept();
    m_tcpServer.run();
}

// ----------------------------------------------------------------------------
// Core Request Handling
// ----------------------------------------------------------------------------

void Controller::requestHandler(const std::string& request,
                                std::string& response) {
    json resp;
    json request_json;

    try {
        request_json = json::parse(request);
    } catch (const std::exception& e) {
        LOG_WARN("CONTROLLER",
                 fmt::format("Failed to parse request: {}", e.what()));
        resp["status"] = "fail";
        response = resp.dump();
        return;
    }

    // Handle special request types (heartbeat, disconnect)
    if (request_json.contains("type")) {
        std::string req_type = request_json["type"];

        if (req_type == "heartbeat") {
            std::string node_id = request_json["id"];
            auto it = m_nodeMap.find(node_id);
            if (it != m_nodeMap.end()) {
                it->second.updateHeartbeat();
                resp["status"] = "success";
            } else {
                resp["status"] = "fail";
            }
            response = resp.dump();
            return;
        }

        if (req_type == "disconnect") {
            std::string node_id = request_json["id"];
            auto it = m_nodeMap.find(node_id);
            if (it != m_nodeMap.end()) {
                RestoreTXPort(it->second.getConfig().getIpAddress());

                auto transmitter = it->second.getTransmitter();
                if (transmitter) {
                    for (auto& [freq, itm] : m_intermediateMap) {
                        itm->DetachDestination(transmitter);
                    }
                }

                auto receiver = it->second.getReceiver();
                if (receiver) {
                    for (auto& [freq, itm] : m_intermediateMap) {
                        itm->DetachSource(receiver);
                    }
                }

                auto key = !it->second.getConfig().getName().empty()
                               ? it->second.getConfig().getName()
                               : it->second.getConfig().getId();
                auto vhIt = m_vehicleHandlers.find(key);
                if (vhIt != m_vehicleHandlers.end()) {
                    vhIt->second->Stop();
                    m_vehicleHandlers.erase(vhIt);
                }

                auto* db = chem::DBHandler::GetInstance("");
                // Remove empty channels
                for (auto itm_it = m_intermediateMap.begin();
                     itm_it != m_intermediateMap.end();) {
                    if (itm_it->second->isEmpty()) {
                        db->DeleteChannel(itm_it->first);
                        itm_it = m_intermediateMap.erase(itm_it);
                    } else {
                        ++itm_it;
                    }
                }

                db->DeleteNode(it->second);
                m_nodeMap.erase(it);

                LOG_INFO("CONTROLLER",
                         fmt::format("Node \"{}\" disconnected", node_id));
                resp["status"] = "success";
            } else {
                resp["status"] = "fail";
            }
            response = resp.dump();
            return;
        }
    }

    // Normal configuration request
    auto nodeConfig = request_json.get<chem::NodeConfig>();
    Node node(nodeConfig);

    node.getConfig().setIpAddress(m_tcpServer.getRemoteIp());
    nodeConfig.setIpAddress(node.getConfig().getIpAddress());

    auto [node_it, newly_inserted] = GetOrCreateNode(node);
    auto& target_node = node_it->second;

    target_node.refreshFromConfig(nodeConfig, m_vehicleHandlers);

    bool created = AssignULChannel(target_node, nodeConfig);
    created |= AssignDLChannel(target_node, nodeConfig);

    if (!created && !newly_inserted) {
        resp["status"] = "fail";
        if (target_node.compare(nodeConfig)) {
            resp["txPort"] = target_node.getConfig().getTxPortNumber();
            resp["rxPort"] = target_node.getConfig().getRxPortNumber();
            resp["status"] = "success";
            response = resp.dump();
            LOG_WARN("CONTROLLER",
                     fmt::format("Ignored redundant request from {}",
                                 target_node.getConfig().getName()));
            return;
        }
        response = resp.dump();
        return;
    }

    resp["txPort"] = target_node.getConfig().getTxPortNumber();
    resp["rxPort"] = target_node.getConfig().getRxPortNumber();
    resp["status"] = "success";
    response = resp.dump();

    if (newly_inserted) {
        if (!nodeConfig.getChannels().empty()) {
            LOG_INFO("CONTROLLER",
                     fmt::format("Node \"{}\" connected (Type: {})",
                                 target_node.getConfig().getName(),
                                 NodeTypeToString(
                                     target_node.getConfig().getNodeType())));
            LOG_INFO(
                "CONTROLLER",
                fmt::format(
                    "\tIP: {} | TX: {:.3f} MHz | RX: {:.3f} MHz",
                    target_node.getConfig().getIpAddress(),
                    HZ_TO_MHZ(nodeConfig.getChannels().at(0).getTxFreq()),
                    HZ_TO_MHZ(nodeConfig.getChannels().at(0).getRxFreq())));
        }
        LOG_INFO("CONTROLLER",
                 fmt::format("Total nodes: {}", m_nodeMap.size()));
    } else {
        LOG_INFO("CONTROLLER", fmt::format("Node \"{}\" updated info",
                                           target_node.getConfig().getName()));
    }

    // Persist to DB
    AddNode(target_node);
}

void Controller::CheckTimeouts() {
    while (true) {
        std::this_thread::sleep_for(5s);

        for (auto it = m_nodeMap.begin(); it != m_nodeMap.end();) {
            if (!it->second.isAlive(10)) {
                LOG_WARN("CONTROLLER",
                         fmt::format("Node \"{}\" timed out. Removing.",
                                     it->second.getConfig().getName()));

                auto transmitter = it->second.getTransmitter();
                if (transmitter) {
                    for (auto& [freq, itm] : m_intermediateMap)
                        itm->DetachDestination(transmitter);
                }

                auto receiver = it->second.getReceiver();
                if (receiver) {
                    for (auto& [freq, itm] : m_intermediateMap)
                        itm->DetachSource(receiver);
                }

                auto* db = chem::DBHandler::GetInstance("");
                for (auto itm_it = m_intermediateMap.begin();
                     itm_it != m_intermediateMap.end();) {
                    if (itm_it->second->isEmpty()) {
                        db->DeleteChannel(itm_it->first);
                        itm_it = m_intermediateMap.erase(itm_it);
                    } else {
                        ++itm_it;
                    }
                }

                db->DeleteNode(it->second);

                auto key = !it->second.getConfig().getName().empty()
                               ? it->second.getConfig().getName()
                               : it->second.getConfig().getId();
                auto vhIt = m_vehicleHandlers.find(key);
                if (vhIt != m_vehicleHandlers.end()) {
                    vhIt->second->Stop();
                    m_vehicleHandlers.erase(vhIt);
                }

                it = m_nodeMap.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Node Management
// ----------------------------------------------------------------------------

std::pair<std::map<std::string, Node>::iterator, bool>
Controller::GetOrCreateNode(const Node& node) {
    auto result = m_nodeMap.insert({node.getConfig().getId(), node});
    if (result.second) {
        AddNode(result.first->second);
    }
    return result;
}

void Controller::AddNode(const Node& node) {
    auto* db = chem::DBHandler::GetInstance("");
    db->AddNode(node);
}

// ----------------------------------------------------------------------------
// Channel Management
// ----------------------------------------------------------------------------

bool Controller::AssignULChannel(Node& node, chem::NodeConfig newConfig) {
    if (newConfig.getChannels().empty()) return false;

    const bool sandbox = RuntimeConfig::sandbox_enabled.load();
    const double freq = newConfig.getChannels().at(0).getTxFreq();
    const double prevFreq =
        node.getConfig().getChannels().empty()
            ? 0.0
            : node.getConfig().getChannels().at(0).getTxFreq();

    intermediateSPtr ch_ptr, prevCh_ptr;
    bool ch_found = FindChannel(freq, ch_ptr);
    bool prev_ch_found = FindChannel(prevFreq, prevCh_ptr);

    bool isSame = node.compare(newConfig);

    // Receiver management is skipped in sandbox mode: no IQ is transported.
    receiverSPtr receiver;
    if (!sandbox) {
        receiver = node.getReceiver();
        if (!receiver) {
            receiver = CreateReceiver(node);
            node.setReceiver(receiver);
        }
        receiver->getNodeConfig().setInputFormat(
            node.getConfig().getInputFormat());
        receiver->getNodeConfig().setOutputFormat(
            node.getConfig().getOutputFormat());
    }

    if (!ch_found) {
        ch_ptr = CreateChannel(freq, newConfig.getSampleRate().getTxRate());
    }

    if (isSame && !ch_found) {
        return false;
    }

    bool freqChanged = (std::abs(prevFreq - freq) >= 10e3);

    if (freqChanged && prev_ch_found) {
        if (sandbox) {
            prevCh_ptr->removeSandboxNode(node.getConfig().getId());
        } else {
            prevCh_ptr->DetachSource(receiver);
            LOG_DEBUG("CONTROLLER",
                      fmt::format("{:.1f} MHz Receiver released",
                                  prevFreq / 1e6));
        }
        isChannelEmpty(prevCh_ptr);
    }

    if (!freqChanged) {
        node.getConfig().setSampleRate(newConfig.getSampleRate());
        if (!sandbox) {
            receiver->setSampleRate(
                node.getConfig().getSampleRate().getTxRate());
        }
        return false;
    }

    node.getConfig().updateTxFreq(freq, 0);
    if (sandbox) {
        ch_ptr->addSandboxNode(node.getConfig().getId(),
                               newConfig.getNumChannels());
    } else {
        receiver->updateFreq(freq);
        receiver->setSampleRate(node.getConfig().getSampleRate().getTxRate());
        ch_ptr->AttachSource(receiver);
    }

    return true;
}

bool Controller::AssignDLChannel(Node& node, chem::NodeConfig newConfig) {
    if (newConfig.getChannels().empty()) return false;

    const bool sandbox = RuntimeConfig::sandbox_enabled.load();
    const double freq = newConfig.getChannels().at(0).getRxFreq();
    const double prevFreq =
        node.getConfig().getChannels().empty()
            ? 0.0
            : node.getConfig().getChannels().at(0).getRxFreq();

    intermediateSPtr ch_ptr, prevCh_ptr;
    bool ch_found = FindChannel(freq, ch_ptr);
    bool prev_ch_found = FindChannel(prevFreq, prevCh_ptr);

    bool isSame = node.compare(newConfig);

    // Transmitter management is skipped in sandbox mode: no IQ is transported.
    transmitterSPtr transmitter;
    if (!sandbox) {
        transmitter = node.getTransmitter();
        if (!transmitter) {
            transmitter = CreateTransmitter(node);
            node.setTransmitter(transmitter);
        }
        transmitter->getNodeConfig().setInputFormat(
            node.getConfig().getInputFormat());
        transmitter->getNodeConfig().setOutputFormat(
            node.getConfig().getOutputFormat());
    }

    if (!ch_found) {
        ch_ptr = CreateChannel(freq, newConfig.getSampleRate().getRxRate());
    }

    if (isSame && !ch_found) {
        return false;
    }

    bool freqChanged = (std::abs(prevFreq - freq) >= 10e3);

    if (prev_ch_found && freqChanged) {
        if (sandbox) {
            prevCh_ptr->removeSandboxNode(node.getConfig().getId());
        } else {
            prevCh_ptr->DetachDestination(transmitter);
            LOG_DEBUG("CONTROLLER",
                      fmt::format("{:.1f} MHz Transmitter released",
                                  prevFreq / 1e6));
        }
        isChannelEmpty(prevCh_ptr);
    }

    if (!freqChanged) {
        node.getConfig().setSampleRate(newConfig.getSampleRate());
        if (!sandbox) {
            transmitter->setSampleRate(
                node.getConfig().getSampleRate().getRxRate());
        }
        return false;
    }

    node.getConfig().updateRxFreq(freq, 0);
    if (sandbox) {
        ch_ptr->addSandboxNode(node.getConfig().getId(),
                               newConfig.getNumChannels());
    } else {
        transmitter->updateFreq(freq);
        transmitter->setSampleRate(
            node.getConfig().getSampleRate().getRxRate());
        ch_ptr->AttachDestination(transmitter);
    }

    return true;
}

intermediateSPtr Controller::CreateChannel(const double& freq,
                                           const double& srate) {
    intermediateSPtr existing;
    if (FindChannel(freq, existing)) return existing;

    auto intermediate =
        std::make_shared<Intermediate>(freq, payload_pool, m_iq_pools);
    LOG_INFO("CONTROLLER",
             fmt::format("Created channel for {:.3f} MHz", HZ_TO_MHZ(freq)));

    // Apply propagation defaults
    PropagationModel defaultModel = PropagationModel::FREE_SPACE;
    float defaultGroundCoeff = -1.0f;
    std::string defaultScenario = "UMa";

    if (m_propagationDefaults) {
        defaultModel = m_propagationDefaults->model.load();
        defaultGroundCoeff = m_propagationDefaults->groundCoeff.load();
        std::lock_guard<std::mutex> lock(m_propagationDefaults->scenarioMutex);
        defaultScenario = m_propagationDefaults->scenario;
    }

    if (defaultModel != PropagationModel::FREE_SPACE &&
        defaultModel != PropagationModel::UNKNOWN) {
        intermediate->updatePathLoss(defaultModel, defaultGroundCoeff);
        if (defaultModel == PropagationModel::THREE_GPP_38_901) {
            intermediate->set3gppScenario(defaultScenario);
        }
    }

    m_intermediateMap.insert({freq, intermediate});

    auto* db = chem::DBHandler::GetInstance("");
    db->AddChannel(freq, PropagationModelToString(defaultModel), srate);

    // Start channel processing loop in high-priority thread
    std::thread inter_thread(&Intermediate::Start, intermediate);
    pthread_t native_handle = inter_thread.native_handle();
    sched_param sched_params;
    sched_params.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(native_handle, SCHED_FIFO, &sched_params);
    inter_thread.detach();

    m_numChannels++;
    return intermediate;
}

bool Controller::FindChannel(const double& freq, intermediateSPtr& channel) {
    auto it = m_intermediateMap.find(freq);
    if (it != m_intermediateMap.end()) {
        channel = it->second;
        return true;
    }
    return false;
}

void Controller::isChannelEmpty(intermediateSPtr& intermediate) {
    if (intermediate && intermediate->isEmpty()) {
        double freq = intermediate->GetFreq();
        m_intermediateMap.erase(freq);
        auto* db = chem::DBHandler::GetInstance("");
        db->DeleteChannel(freq);
    }
}

// ----------------------------------------------------------------------------
// Resource Creation
// ----------------------------------------------------------------------------

transmitterSPtr Controller::CreateTransmitter(Node& node) {
    node.getConfig().setRxPortNumber(
        AssignRXPort(node.getConfig().getIpAddress()));

    auto transmitter =
        std::make_shared<Transmitter>(io_service, node.getConfig(), iq_pool);
    std::thread tx_thread(&Transmitter::Start, transmitter);
    tx_thread.detach();

    if (!node.getConfig().getChannels().empty()) {
        LOG_INFO("CONTROLLER",
                 fmt::format(
                     "Transmitter created for \"{}\" (UHD-RX) on {:.1f} MHz",
                     node.getConfig().getName(),
                     node.getConfig().getChannels().at(0).getRxFreq() / 1e6));
    }

    return transmitter;
}

receiverSPtr Controller::CreateReceiver(Node& node) {
    node.getConfig().setTxPortNumber(
        AssignTXPort(node.getConfig().getIpAddress()));

    auto receiver =
        std::make_shared<Receiver>(io_service, node.getConfig(), iq_pool);

    // Start receiver thread with high priority
    std::thread rx_thread(&Receiver::Start, receiver);
    pthread_t native_handle = rx_thread.native_handle();
    sched_param sched_params;
    sched_params.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(native_handle, SCHED_FIFO, &sched_params);
    rx_thread.detach();

    if (!node.getConfig().getChannels().empty()) {
        LOG_INFO("CONTROLLER",
                 fmt::format(
                     "Receiver created for \"{}\" (UHD-TX) on {:.1f} MHz",
                     node.getConfig().getName(),
                     node.getConfig().getChannels().at(0).getTxFreq() / 1e6));
    }

    return receiver;
}

// ----------------------------------------------------------------------------
// Port Management
// ----------------------------------------------------------------------------

void Controller::GeneratePortPool(uint16_t portEnd,
                                  std::vector<uint16_t>& portPool) {
    std::generate(portPool.begin(), portPool.end(),
                  [&portEnd] { return portEnd--; });
}

uint16_t Controller::AssignTXPort(const std::string& ipAddress) {
    if (m_txPortPool.empty()) return 0;
    uint16_t portNum = m_txPortPool.back();
    m_txPortPool.pop_back();
    return portNum;
}

uint16_t Controller::AssignRXPort(const std::string& ipAddress) {
    if (m_rxPortPool.empty()) return 0;
    uint16_t portNum = m_rxPortPool.back();
    m_rxPortPool.pop_back();
    return portNum;
}

void Controller::RestoreTXPort(const std::string& ipAddress) {
    auto it = m_ipAddrMap.find(ipAddress);
    if (it != m_ipAddrMap.end()) {
        if (--(it->second) == 0) {
            m_ipAddrMap.erase(it);
        }
    }
}

}  // namespace emulator
}  // namespace chem
