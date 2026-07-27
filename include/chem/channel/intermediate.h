/**
 * @file intermediate.h
 * @brief Channel-Intermadiate for CHEM
 * @author Anıl Gürses
 * @version v1.0
 */

#pragma once

#include <tbb/concurrent_queue.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <tuple>
#include <uhd/convert.hpp>

#include "../common.h"
#include "../models/data_pool.hpp"
#include "../models/signal.hpp"
#include "../nlohmann/json.hpp"
#include "../node/receiver.h"
#include "../node/transmitter.h"
#include "channel.h"
#define N_CH 4  // Max number of channels to receive
#define MAX_NUM_SAMPLES \
    100000  // Max number of samples per channel, usually 365 (1460/4), only
            // allowing 12x
// upsampling
#define CONV_SCALAR 32767.0f           // Scalar for conversion
#define CONV_SCALAR_I 1.0f / 32767.0f  // Scalar for conversion

namespace chem {

using transmitterSPtr = std::shared_ptr<emulator::Transmitter>;
using receiverSPtr = std::shared_ptr<emulator::Receiver>;

using ChannelTimingCallback = std::function<void(const ChannelProcessTiming&)>;
using SignalProcessingCallback = std::function<void(int64_t duration_ns)>;

class Intermediate {
   public:
    struct DestInfo {
        std::string dest_id;
        float sample_rate;
        size_t num_channels;
        const NodeConfig* config;
        transmitterSPtr transmitter;
        Channel* channel{nullptr};
    };

    /// Per-signal context threaded through processing functions.
    struct SignalContext {
        struct Header sig_hdr;
        const std::string* src_id;
        size_t src_n_ch;
        size_t i_sz_ch;
        size_t i_ns_ch;
        float sig_srate;
        int64_t sig_t_recv{0};
        int64_t sig_t_proc_start{0};
        int64_t t_conversion_rx{0};
        int64_t t_lock_acquire{0};
        int64_t t_dest_snapshot{0};
    };

    // Per-receiver worker context for parallel processing
    struct WorkerContext {
        std::thread worker_thread;
        tbb::concurrent_bounded_queue<std::unique_ptr<chem::Signal>> queue;
        std::vector<signal_v> src_sig_arr;
        uhd::convert::converter::sptr converter_rx;
        std::atomic<bool> running{true};
        std::shared_ptr<std::atomic<bool>> running_flag;
        std::string receiver_id;
        std::string src_node_id;
        const NodeConfig* src_config{nullptr};
        std::vector<DestInfo> destinations;
        uint64_t cached_dest_version{0};
        std::vector<DestInfo> cached_destinations;
        std::vector<dArray_uptr> pre_acquired_buffers;
        int numa_node{0};
        std::atomic<int64_t> last_tx_ns{0};

        WorkerContext() { queue.set_capacity(QUEUE_CAP); }

        void ensureSrcChannels(size_t n) {
            if (n > src_sig_arr.size())
                src_sig_arr.resize(n,
                                   signal_v(MAX_NUM_SAMPLES, fc(0.0f, 0.0f)));
        }
    };

    Intermediate() = delete;

    Intermediate(const double& freq, payloadPoolSPtr pool,
                 std::vector<iqPoolSPtr> iq_pools,
                 int64_t max_latency_ms = 10);

    ~Intermediate();

    void Start();

    void ProcessSignal(WorkerContext& ctx, std::unique_ptr<chem::Signal> sig);

    void AttachDestination(transmitterSPtr transmitter);
    bool DetachDestination(transmitterSPtr transmitter);

    void AttachSource(receiverSPtr receiver);
    bool DetachSource(receiverSPtr receiver);

    bool isEmpty() const;

    void add2Queue(std::unique_ptr<chem::Signal> sig);

    // TODO:
    void addSamples();

    uint8_t GetDestinationCnt() const;

    uint8_t GetSourceCnt() const;

    void updateChannelMap();

    void setShadowingSTD(const double& std);

    void removeChannel(const std::string& nodeId);

    std::map<std::pair<std::string, std::string>, chem::Channel>::iterator
    getChannel(const std::string& source, const std::string& dest);

    std::map<std::pair<std::string, std::string>, chem::Channel>&
    getChannelList();

    template <typename F>
    void withChannelsExclusive(F&& fn) {
        std::unique_lock<std::shared_mutex> guard(dest_mutex);
        fn(m_channelMap);
    }

    void addNewChannel(const std::string& source, const std::string& dest);

    // PyCHEM Methods
    bool updateChannelCoeff(const std::string& src, const std::string& dest,
                            const struct chId& ch, const float& coeff);

    bool updateDistance(const std::string& src, const std::string& dest,
                        const float& distance);

    bool updateAltitude(const std::string& src, const std::string& dest,
                        const float& altitudeDiff);

    bool updateElevation(const std::string& src, const std::string& dest,
                         const float& elevation);

    bool updateAzimuth(const std::string& src, const std::string& dest,
                       const float& azimuth);

    bool updateFrequencyOffset(const std::string& src, const std::string& dest,
                               const double& freq_offset_hz);

    bool updateDopplerEnabled(const std::string& src, const std::string& dest,
                              const bool& enabled);

    bool updateDopplerHz(const std::string& src, const std::string& dest,
                         const double& doppler_hz);

    void updatePathLoss(const std::string& plMode, const float& gr_coeff = -1);
    void updatePathLoss(PropagationModel plMode, const float& gr_coeff = -1);

    bool updateAWGN(const std::string& src, const std::string& dest,
                    const bool& awgnEnabled, const float& a_snr);

    bool updateCIR(const std::string& src, const std::string& dest,
                   const struct chId& ch, const signal_v& taps);

    // PyCHEM Methods

    void ApplyTxAntenna();

    void ApplyRxAntenna();

    const double GetFreq() const;

    PropagationModel getPathLossMode() const;

    void setPathLossMode(const std::string& plMode);
    void setPathLossMode(PropagationModel plMode);
    void set3gppScenario(const std::string& scenario);
    void setHataEnvironment(const std::string& environment);
    void setITMParams(float refractivity, float ground_conductivity,
                      float ground_permittivity, int climate_zone);

    void setMaxLatency(int64_t max_latency_ms);
    int64_t getMaxLatency() const;

    // Update antenna pattern for a node by name or ID
    bool updateNodeAntenna(const std::string& nodeName,
                           const std::string& pattern,
                           const std::string& txPattern = "",
                           const std::string& rxPattern = "");

    /**
     * @brief Enable detailed channel timing collection
     * @param callback Function to call with timing data after each
     * processChannel()
     *
     * When enabled, timing data for each channel impairment step (resample,
     * CIR, pathloss, noise, freq_offset) is collected and passed to the
     * callback.
     */
    void setTimingCallback(ChannelTimingCallback callback);

    void clearTimingCallback();

    bool isTimingEnabled() const;

    /**
     * @brief Set callback for per-input-signal processing time
     *
     * Fires once per input signal after all destination processing completes.
     * Measures wall-clock time for the entire ProcessSignal call.
     */
    void setSignalProcessingCallback(SignalProcessingCallback callback);

    void clearSignalProcessingCallback();

    // Number of output signals dropped due to pool exhaustion in
    // ProcessSignal (acquireBatch failure after input buffer was released).
    size_t getPoolDropCount() const {
        return m_poolDropCount.load(std::memory_order_relaxed);
    }

    // Number of signals dropped in WorkerLoop because they exceeded
    // the maximum latency threshold (too old).
    size_t getWorkerAgeDropCount() const {
        return m_workerAgeDropCount.load(std::memory_order_relaxed);
    }

    void resetDropCounters() {
        m_poolDropCount.store(0, std::memory_order_relaxed);
        m_workerAgeDropCount.store(0, std::memory_order_relaxed);
    }

    // Sandbox mode: register virtual nodes that have no Rx/Tx endpoints.
    // The channel cross-product is built from the sandbox node set so that
    // path-loss and antenna calculations can still run.
    void addSandboxNode(const std::string& nodeId, size_t numChannels);
    void removeSandboxNode(const std::string& nodeId);
    const std::set<std::string>& getSandboxNodes() const;

    std::vector<std::pair<std::string, int64_t>> getTxActivity() const;

   private:
    void EmptyChannelChecker();

    PathLossType currentPlType() const;

    void insertChannelIfMissing(const std::string& src_id,
                                const std::string& dest_id,
                                uint8_t destNumCh,
                                uint8_t srcNumCh);

    void printStatus();

    void WorkerLoop(WorkerContext& ctx);

    void InitWorkerConverters(WorkerContext& ctx);

    void ProcessDestinationSignal(size_t dest_index, WorkerContext& ctx,
                                  const std::vector<DestInfo>& destinations,
                                  const SignalContext& sig_ctx,
                                  const ChannelProcessParams& params,
                                  dArray_uptr pre_acquired_buffer = nullptr);

   private:
    std::atomic<bool> work{true};
    double m_freq;
    int64_t m_max_latency_ns;  // Max signal age before dropping

    PropagationModel m_propagationModel = PropagationModel::FREE_SPACE;

    float grReflection = -1.0f;
    std::string m_3gppScenario = "UMa";
    std::string m_hataEnvironment = "URBAN";
    float m_itmRefractivity = 301.0f;
    float m_itmGroundConductivity = 0.005f;
    float m_itmGroundPermittivity = 15.0f;
    int m_itmClimateZone = 5;

    std::mutex src_mutex;
    mutable std::shared_mutex dest_mutex;
    std::condition_variable cv;

    std::vector<iqPoolSPtr> m_iq_pools;

    std::unordered_map<std::string, std::shared_ptr<emulator::Receiver>>
        m_sourceMap;
    std::unordered_map<std::string, std::shared_ptr<emulator::Transmitter>>
        m_destinationMap;
    std::atomic<size_t> m_destCount{0};
    std::atomic<uint64_t> m_destVersion{0};

    std::map<std::pair<std::string, std::string>, chem::Channel> m_channelMap;

    // Per-receiver worker contexts for parallel processing
    std::unordered_map<std::string, std::unique_ptr<WorkerContext>> m_workers;
    mutable std::mutex m_workers_mutex;

    ChannelTimingCallback m_timingCallback;
    std::atomic<bool> m_timingEnabled{false};
    mutable std::mutex m_timingMutex;

    SignalProcessingCallback m_signalProcessingCallback;
    std::atomic<bool> m_signalProcessingEnabled{false};

    std::atomic<size_t> m_poolDropCount{0};
    std::atomic<size_t> m_workerAgeDropCount{0};

    // Sandbox mode: virtual nodes without Rx/Tx endpoints
    std::set<std::string> m_sandboxNodes;
    std::unordered_map<std::string, size_t> m_sandboxNumChannels;
};
}  // namespace chem
