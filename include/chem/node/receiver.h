#pragma once

#include <string.h>
#include <tbb/concurrent_queue.h>

#include <boost/asio/io_service.hpp>
#include <complex>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>

#include "../common.h"
#include "../models/signal.hpp"
#include "../net/tcp_client.h"
#include "../net/udp_server.h"
#include "../node/node_entity.h"
#include "chem/models/data_pool.hpp"
#include "spdlog/spdlog.h"

#define UDP

namespace chem {
namespace emulator {
class Receiver {
   public:
    Receiver(boost::asio::io_service& io_service, const NodeConfig& node_config,
             iqPoolSPtr iq_pool, uint8_t ch = 0);

    ~Receiver();

    void Start();

    void setSampleRate(const uint32_t rate);
    uint32_t getSampleRate() const;

    void setBytesPerSample(uint8_t bytes);

    void setAddQueFnc(std::function<void(std::unique_ptr<chem::Signal>)> add);
    void Add2Buff(std::unique_ptr<chem::Signal> sig);
    std::unique_ptr<chem::Signal> getSignalFromQueue();

    size_t getQueueSize() const;

    void setAttachState(const bool& update);
    bool isAttached() const;

    const std::string getId() const;
    const std::string getName() const;

    bool isWorking() const;

    const double getFreq() const;
    void updateFreq(const double& freq);

    uint8_t getNumChannels() const;

    const NodeConfig& getNodeConfig() const;
    NodeConfig& getNodeConfig();

    const uint16_t getPortNum() const;

   private:
    NodeConfig m_node_config;
    std::mutex m_mutex;

    std::string m_NodeId;
    std::string m_NodeName;

    uint8_t ch_no;

    std::unique_ptr<char[]> buff;
    std::deque<NodeConfig> m_nodeQueue;

    std::string m_ip_address;
    uint16_t m_portNum;

    bool _sent = false;
    bool _working = true;
    bool m_attached = false;
    bool _queuing = false;

    uint32_t m_sample_rate;
    double m_freq;
    uint8_t m_numChannels;

    std::function<void(std::unique_ptr<chem::Signal>)> add2Queue;

    tbb::concurrent_bounded_queue<std::unique_ptr<chem::Signal>> m_queue;

    iqPoolSPtr m_iq_pool;

    boost::asio::io_service& m_io_service;

#ifndef UDP
    chem::Client m_client;
#else
    chem::UDPServer m_server;
#endif  // !UDP
};
}  // namespace emulator
}  // namespace chem
