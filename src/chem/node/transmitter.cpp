#include "chem/node/transmitter.h"

#include <chem/common.h>
#include <spdlog/fmt/fmt.h>

#include <boost/asio/io_service.hpp>
#include <cstring>
#include <mutex>

#include "chem/models/data_pool.hpp"

#define UDP

using namespace chem;
using namespace chem::emulator;
using namespace std::chrono;
using namespace std::chrono_literals;

Transmitter::Transmitter(boost::asio::io_service& io_service,
                         const NodeConfig& node_config, iqPoolSPtr iq_pool)
    : m_io_service(io_service),
      m_iq_pool(iq_pool),
      m_ip_address(node_config.getIpAddress()),
      m_bytes_per_sample(4),
      m_client(node_config.getIpAddress(), node_config.getRxPortNumber(),
               iq_pool),
      m_sample_rate(node_config.getSampleRate().getRxRate()),
      m_freq(node_config.getChannels().at(0).getRxFreq()),
      m_NodeId(node_config.getId()),
      m_NodeName(node_config.getName()),
      m_numChannels(node_config.getNumChannels()),
      m_node_config(node_config) {
    m_portNum = node_config.getRxPortNumber();
}

const NodeConfig& Transmitter::getNodeConfig() const { return m_node_config; }

NodeConfig& Transmitter::getNodeConfig() { return m_node_config; }

Transmitter::~Transmitter() {
    LOG_DEBUG("Transmitter",
              fmt::format("Transmitter for frequency {} is now being closed!",
                          m_freq));
}

void Transmitter::Start() {
    try {
        LOG_INFO(
            "TRANSMITTER",
            fmt::format(
                "CHEM-Transmitter NODE {} is created on {} port for {} MHz",
                m_NodeName, m_portNum, m_freq / 1e6));
    } catch (std::exception& e) {
        LOG_ERROR(
            "TRANSMITTER",
            fmt::format("Couldn't create a socket with cause : {}", e.what()));
        return;
    }

#ifdef ENABLE_DEBUG_LOG
    int d_cnt = 0;
    float avg = 0;

    float avg_r_p_start = 0;
    float avg_r_p_end = 0;
    float avg_p_end_t = 0;
#endif

    while (_working) {
        std::unique_ptr<chem::Signal> t_sig;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait(lock,
                            [this] { return !m_queue.empty() || !_working; });
            if (!_working && m_queue.empty()) {
                break;
            }
        }
        if (!m_queue.try_pop(t_sig)) {
            continue;
        }

#ifdef ENABLE_DEBUG_LOG
        Header hdr = *(t_sig->getHeader());

        // Get time difference
        auto now = steady_clock::now();
        int64_t now_cnt =
            duration_cast<nanoseconds>(now.time_since_epoch()).count();
        t_sig->setTSend(now_cnt);
        const int64_t t_tx = hdr.rt_tx_time;  // ns

        avg += now_cnt - t_tx;
        avg_r_p_start += t_sig->getTProcStart() - t_sig->getTRecv();
        avg_r_p_end += t_sig->getTProcEnd() - t_sig->getTProcStart();
        avg_p_end_t += t_sig->getTSend() - t_sig->getTProcEnd();
        d_cnt++;

        if (d_cnt % 2000 == 0) {
            LOG_INFO(fmt::format("DELAY {}", m_NodeName),
                     fmt::format(
                         "Recv-Start: {:.2f} us | Start-End: {:.2f} us | "
                         "End-Tx: {:.2f} us | Tot: {:.2f} us",
                         NS_2_US(avg_r_p_start / d_cnt),
                         NS_2_US(avg_r_p_end / d_cnt),
                         NS_2_US(avg_p_end_t / d_cnt), NS_2_US(avg / d_cnt)));
            d_cnt = 0;
            avg = 0;
            avg_r_p_start = 0;
            avg_r_p_end = 0;
            avg_p_end_t = 0;
        }
#endif
        auto send_clock = steady_clock::now();
        auto send_time =
            duration_cast<nanoseconds>(send_clock.time_since_epoch()).count();

        if (t_sig->getHeader()->start < (send_time - MS_2_NS(5))) {
            m_iq_pool->release(t_sig->dataArray());
#ifdef ENABLE_DEBUG_LOG
            LOG_WARN("TRANSMITTER",
                     fmt::format("Signal is too old to be transmitted! "
                                 "Diff: {:.2f} ms | Start: {} | Now: {}",
                                 NS_2_MS(send_time - t_sig->getHeader()->start),
                                 t_sig->getHeader()->start, send_time));
#endif
            continue;
        }

        try {
            m_client.Send(std::move(t_sig), m_bytes_per_sample);
        } catch (std::exception& e) {
            LOG_WARN("TRANSMITTER", "Node is not connected anymore");
            m_iq_pool->release(std::move(t_sig->dataArray()));
            _working = false;
            break;
        }
    }
}

void Transmitter::setSampleRate(uint32_t rate) {
    m_sample_rate = rate;
    usrp::SampleRate sr = m_node_config.getSampleRate();
    sr.setRxRate(rate);
    m_node_config.setSampleRate(sr);
}

void Transmitter::setBytesPerSample(uint8_t bytes) {
    m_bytes_per_sample = bytes;
}

uint32_t Transmitter::getSampleRate() const { return m_sample_rate; }

const uint16_t Transmitter::getPortNum() const { return m_portNum; }

void Transmitter::Add2Buff(std::unique_ptr<chem::Signal> sig) {
    if (m_queue.size() >= (QUEUE_CAP - 1)) {
        LOG_WARN("TRANSMITTER", "TX queue is full, dropping a packet");
        // Pop the oldest (earliest timestamp) packet to make room
        std::unique_ptr<chem::Signal> oldest;
        if (m_queue.try_pop(oldest)) {
            m_iq_pool->release(std::move(oldest->dataArray()));
        }
        m_iq_pool->release(std::move(sig->dataArray()));
        return;
    }
    m_queue.push(std::move(sig));
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
    }
    m_queue_cv.notify_one();
}

size_t Transmitter::getQueueSize() const { return m_queue.size(); }

bool Transmitter::isAttached() const { return m_attached; }

void Transmitter::setAttachState(const bool& update) { m_attached = update; }

const std::string Transmitter::getId() const { return m_NodeId; }

const std::string Transmitter::getName() const { return m_NodeName; }

bool Transmitter::isWorking() const { return _working; }

void Transmitter::updateFreq(const double& freq) {
    m_freq = freq;
    LOG_INFO(
        "TRANSMITTER",
        fmt::format("Center frequency is updated to {} MHz!", HZ_TO_MHZ(freq)));
}

const double Transmitter::getFreq() const { return m_freq; }

const uint8_t Transmitter::getNumChannels() const { return m_numChannels; }
