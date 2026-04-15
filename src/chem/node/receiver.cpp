#include "chem/node/receiver.h"

#include <chem/common.h>
#include <spdlog/fmt/fmt.h>

#include <boost/asio/io_service.hpp>
#include <memory>
#include <mutex>

#include "chem/models/data_pool.hpp"

using namespace chem;
using namespace chem::emulator;
using namespace std::chrono;
using namespace std::chrono_literals;

#define UDP

Receiver::Receiver(boost::asio::io_service& io_service,
                   const NodeConfig& node_config, iqPoolSPtr iq_pool,
                   uint8_t ch)
    : m_io_service(io_service),
      m_iq_pool(iq_pool),
      m_NodeId(node_config.getId()),
      m_sample_rate(node_config.getSampleRate().getTxRate()),
      m_freq(node_config.getChannels().at(0).getTxFreq()),
      m_ip_address(node_config.getIpAddress()),
      m_NodeName(node_config.getName()),
      ch_no(ch),
      _working(true),
      m_numChannels(node_config.getNumChannels()),
      m_node_config(node_config)
#ifndef UDP
      ,
      m_client(m_ip_address, std::get<0>(node_config.getTxPortNumber()))
#else
      ,
      m_server(m_ip_address, node_config.getTxPortNumber(), iq_pool)
#endif  // !UDP
{
    m_queue.set_capacity(QUEUE_CAP);
    m_portNum = node_config.getTxPortNumber();
}

const NodeConfig& Receiver::getNodeConfig() const { return m_node_config; }

NodeConfig& Receiver::getNodeConfig() { return m_node_config; }

Receiver::~Receiver() {
    LOG_DEBUG("RECEIVER", "CHEM-Receiver is being destroyed");
}

void Receiver::Start() {
    try {
        LOG_INFO("RECEIVER",
                 fmt::format("CHEM-Receiver NODE \"{}\" Ch {} binded to "
                             "udp://0.0.0.0:{} on {} MHz",
                             m_NodeName, ch_no, m_portNum, m_freq / 1000000));

    } catch (std::exception& e) {
        LOG_INFO("RECEIVER", fmt::format("Couldn't create a CHEM-Receiver port "
                                         "with cause : {} Port : {}",
                                         e.what(), m_portNum));
        return;
    }

    std::unique_ptr<char[]> rx;

#ifdef ENABLE_DEBUG_LOG
    double avg = 0;
    double avg_total = 0;
    int d_cnt = 0;
    int64_t last_rx = 0;
#endif

    while (_working) {
        struct Header hdr;

#ifdef ENABLE_DEBUG_LOG
        auto t_start = steady_clock::now();
        int64_t t_start_cnt =
            duration_cast<nanoseconds>(t_start.time_since_epoch()).count();
#endif

        try {
            rx = m_server.Recv(hdr);
        } catch (std::exception& e) {
            LOG_ERROR("RECEIVER",
                      fmt::format("Couldn't receive due to : {}", e.what()));
            _working = false;
            break;
        }

        if (hdr.size == 0) {
            continue;
        }

        if (m_iq_pool->available() < 5) {
            LOG_WARN("RECEIVER",
                     fmt::format("Pool is almost empty {} {}",
                                 m_iq_pool->available(), m_queue.size()));
        }

        if (m_queue.size() >= QUEUE_CAP) {
            std::unique_ptr<chem::Signal> _temp;
            if (m_queue.try_pop(_temp)) {
                LOG_DEBUG("RECEIVER",
                          fmt::format("Queue is full, releasing the oldest "
                                      "packet {} Queue size {}",
                                      m_iq_pool->available(), m_queue.size()));
                m_iq_pool->release(std::move(_temp->dataArray()));
            }
        }

        auto t_sig =
            std::make_unique<Signal>(std::move(rx), hdr, m_freq, m_NodeId);

#ifdef ENABLE_DEBUG_LOG
        auto end = steady_clock::now();
        auto end_cnt =
            duration_cast<nanoseconds>(end.time_since_epoch()).count();
        t_sig->setTRecv(end_cnt);
        const int64_t t_tx = t_sig->getHeader()->start;  // ns

        if (t_tx < end_cnt - MS_2_NS(5)) {
            LOG_WARN("RECEIVER",
                     fmt::format("Signal is too old to be processed! "
                                 "Start: {} | Now: {}",
                                 t_sig->getHeader()->start, t_start_cnt));
        }
#endif

        // If attached to intermediate, push directly to its queue for faster
        // processing
        if (add2Queue) {
            add2Queue(std::move(t_sig));
        } else if (!m_queue.try_push(std::move(t_sig))) {
            LOG_DEBUG(
                "RECEIVER",
                fmt::format(
                    "Queue is full, releasing the packet {} Queue size {}",
                    m_iq_pool->available(), m_queue.size()));
            m_iq_pool->release(std::move(t_sig->dataArray()));
        }

#ifdef ENABLE_DEBUG_LOG
        auto now = steady_clock::now();
        int64_t now_cnt =
            duration_cast<nanoseconds>(now.time_since_epoch()).count();

        avg += NS_2_US(now_cnt - t_tx);
        avg_total += NS_2_US(now_cnt - t_start_cnt);
        d_cnt++;
        if (d_cnt % 200000 == 0) {
            LOG_DEBUG(
                "DELAY RX",
                fmt::format("RT {:.4f} us | RECV Queue Add Delay {:.4f} us",
                            avg / d_cnt, avg_total / d_cnt));
            avg = 0;
            d_cnt = 0;
            avg_total = 0;
        }
#endif
    }
}

std::unique_ptr<chem::Signal> Receiver::getSignalFromQueue() {
    std::unique_ptr<chem::Signal> buff_it;

    if (m_queue.try_pop(buff_it)) {
        return buff_it;
    }

    return nullptr;
}

size_t Receiver::getQueueSize() const { return m_queue.size(); }

void Receiver::setAddQueFnc(
    std::function<void(std::unique_ptr<chem::Signal>)> add) {
    add2Queue = add;
}

void Receiver::Add2Buff(std::unique_ptr<chem::Signal> sig) {
    // If attached to intermediate, route to worker queue via callback
    if (add2Queue) {
        add2Queue(std::move(sig));
        return;
    }

    // Otherwise, use local queue (for unattached receivers)
    if (m_queue.size() >= (QUEUE_CAP - 1)) {
        LOG_WARN("RECEIVER", "TX queue is full, dropping a packet");
        std::unique_ptr<chem::Signal> _temp;
        if (m_queue.try_pop(_temp)) {
            m_iq_pool->release(std::move(_temp->dataArray()));
        }
        m_iq_pool->release(std::move(sig->dataArray()));
        return;
    }
    m_queue.push(std::move(sig));
}

void Receiver::setAttachState(const bool& update) { m_attached = update; }

bool Receiver::isAttached() const { return m_attached; }

bool Receiver::isWorking() const { return _working; }

const std::string Receiver::getId() const { return m_NodeId; }

const std::string Receiver::getName() const { return m_NodeName; }

void Receiver::updateFreq(const double& freq) { m_freq = freq; }

void Receiver::setSampleRate(const uint32_t rate) {
    m_sample_rate = rate;
    usrp::SampleRate sr = m_node_config.getSampleRate();
    sr.setTxRate(rate);
    m_node_config.setSampleRate(sr);
}

uint32_t Receiver::getSampleRate() const { return m_sample_rate; }

const double Receiver::getFreq() const { return m_freq; }

uint8_t Receiver::getNumChannels() const { return m_numChannels; }

const uint16_t Receiver::getPortNum() const { return m_portNum; }

void Receiver::setBytesPerSample(uint8_t bytes) {
    // TODO: Implement this
}
