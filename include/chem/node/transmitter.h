/**
 * @file transmitter.h
 * @brief CHEM-Transmitter
 * @author Anıl Gürses
 * @version v1.0
 */

#pragma once

#include <tbb/concurrent_priority_queue.h>

#include <boost/asio.hpp>
#include <boost/asio/io_service.hpp>
#include <condition_variable>
#include <mutex>
#include <uhd/convert.hpp>
#include <uhd/utils/byteswap.hpp>

#include "../channel/intermediateObserver.h"
#include "../common.h"
#include "../models/data_pool.hpp"
#include "../models/signal.hpp"
#include "../nlohmann/json.hpp"
#include "../node/node_entity.h"

#ifdef UDP
#include "../net/udp_client.h"
#else
#include "../net/tcp_server.h"
#endif

using json = nlohmann::json;

namespace chem {
namespace emulator {

// Comparator for TBB priority queue: earlier timestamps have higher priority
// (min-heap)
struct SignalTimestampCompare {
    bool operator()(const std::unique_ptr<chem::Signal>& a,
                    const std::unique_ptr<chem::Signal>& b) const {
        return a->getHeader()->start > b->getHeader()->start;
    }
};

class Transmitter : public IntermediateObserver {
   public:
    Transmitter() = delete;
    Transmitter(boost::asio::io_service& io_service,
                const NodeConfig& node_config, iqPoolSPtr iq_pool);
    ~Transmitter() override;

    void Start();

    void setSampleRate(uint32_t rate);
    uint32_t getSampleRate() const;

    void setBytesPerSample(uint8_t bytes);

    void Add2Buff(std::unique_ptr<chem::Signal> sig) override;
    size_t getQueueSize() const;

    const uint16_t getPortNum() const;

    const std::string getId() const;

    const std::string getName() const;

    bool isWorking() const;

    const double getFreq() const;
    void updateFreq(const double& freq);

    const uint8_t getNumChannels() const;

    bool isAttached() const;
    void setAttachState(const bool& update);

    const NodeConfig& getNodeConfig() const;
    NodeConfig& getNodeConfig();

   private:
    NodeConfig m_node_config;
    std::string m_NodeId;
    std::string m_NodeName;
    NodeType m_NodeType;

    mutable std::mutex m_mutex;
    std::condition_variable _cv_tx;

    uhd::convert::converter::sptr _converter;

    std::deque<std::unique_ptr<chem::Signal>> sig_buff;
    tbb::concurrent_priority_queue<std::unique_ptr<chem::Signal>,
                                   SignalTimestampCompare>
        m_queue;
    mutable std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;

    iqPoolSPtr m_iq_pool;

#ifndef UDP
    chem::Server m_server;
#else
    chem::UDPClient m_client;
#endif  // !UDP

    boost::asio::io_service& m_io_service;

    std::string m_ip_address;
    uint16_t m_portNum;
    uint32_t m_sample_rate;
    uint8_t m_numChannels;
    uint8_t m_bytes_per_sample;
    double m_freq;
    bool _working = true;
    bool first = true;
    bool m_attached = false;
    bool cv_ntf = false;
};
}  // namespace emulator
}  // namespace chem
