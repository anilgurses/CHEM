/**
 * @file achem_benchmark.cpp
 * @brief CHEM Benchmark Tool - Performance testing for channel emulation
 * @author Anil Gurses <agurses@ncsu.edu>
 * @version v1.1
 *
 * Generates signals and measures performance metrics including latency,
 * resource usage, and packet delivery rates across TX/RX configurations.
 *
 */

#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/io_service.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <tbb/global_control.h>
#include <thread>
#include <vector>

#include "chem/channel/channel.h"
#include "chem/numa_utils.h"
#include "chem/channel/intermediate.h"
#include "chem/common.h"
#include "chem/dsp/channel.h"
#include "chem/models/data_pool.hpp"
#include "chem/models/node_config.hpp"
#include "chem/models/signal.hpp"
#include "chem/models/usrp_channel.hpp"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

struct BenchmarkConfig {
    size_t max_connections = 8;
    double benchmark_duration_s = 10.0;
    double warmup_duration_s = 1.0;
    double frequency_hz = 2.68e9;
    uint32_t sample_rate = 11520000;
    uint8_t num_channels = 1;
    size_t signal_duration_us = 1000;
    int64_t max_latency_ms = 5;
    double drop_threshold = 0.1;
    std::string output_file = "achem_benchmark_results.csv";
    bool verbose = false;
    bool max_mode = false;
    bool detailed_timing = false;
    size_t cir_taps = 0;
    size_t runs = 1;
    int numa_node = -1;
};

struct LatencyResult {
    double min_us = 0;
    double avg_us = 0;
    double p50_us = 0;
    double p90_us = 0;
    double p95_us = 0;
    double p99_us = 0;
    double p999_us = 0;
    double max_us = 0;
    size_t count = 0;
};

class LatencyRecorder {
   public:
    void reserve(size_t n) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_samples.reserve(n);
    }

    void record(int64_t ns) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_samples.push_back(ns);
    }

    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_samples.clear();
    }

    LatencyResult compute() {
        std::lock_guard<std::mutex> lock(m_mutex);
        LatencyResult r;
        r.count = m_samples.size();
        if (r.count == 0) return r;

        std::sort(m_samples.begin(), m_samples.end());

        double total = 0;
        for (auto ns : m_samples) total += static_cast<double>(ns);

        r.min_us = static_cast<double>(m_samples.front()) / 1000.0;
        r.max_us = static_cast<double>(m_samples.back()) / 1000.0;
        r.avg_us = total / static_cast<double>(r.count) / 1000.0;
        r.p50_us = percentile(0.50);
        r.p90_us = percentile(0.90);
        r.p95_us = percentile(0.95);
        r.p99_us = percentile(0.99);
        r.p999_us = percentile(0.999);
        return r;
    }

   private:
    double percentile(double p) const {
        if (m_samples.empty()) return 0;
        double idx = p * static_cast<double>(m_samples.size() - 1);
        size_t lo = static_cast<size_t>(idx);
        size_t hi = std::min(lo + 1, m_samples.size() - 1);
        double frac = idx - static_cast<double>(lo);
        double val = static_cast<double>(m_samples[lo]) * (1.0 - frac) +
                     static_cast<double>(m_samples[hi]) * frac;
        return val / 1000.0;
    }

    std::mutex m_mutex;
    std::vector<int64_t> m_samples;
};

struct BenchmarkRecorders {
    // Signal-level latency breakdown
    LatencyRecorder recv_to_proc_start;
    LatencyRecorder proc_start_to_end;
    LatencyRecorder proc_end_to_send;
    LatencyRecorder total_latency;

    // Channel impairment step latencies
    LatencyRecorder latency_resample;
    LatencyRecorder latency_cir;
    LatencyRecorder latency_pathloss;
    LatencyRecorder latency_noise;
    LatencyRecorder latency_freq_offset;

    void reserve(size_t n) {
        recv_to_proc_start.reserve(n);
        proc_start_to_end.reserve(n);
        proc_end_to_send.reserve(n);
        total_latency.reserve(n);
        latency_resample.reserve(n);
        latency_cir.reserve(n);
        latency_pathloss.reserve(n);
        latency_noise.reserve(n);
        latency_freq_offset.reserve(n);
    }

    void reset() {
        recv_to_proc_start.reset();
        proc_start_to_end.reset();
        proc_end_to_send.reset();
        total_latency.reset();
        latency_resample.reset();
        latency_cir.reset();
        latency_pathloss.reset();
        latency_noise.reset();
        latency_freq_offset.reset();
    }

    void computeAll(struct ScenarioStats& stats);
};

struct ScenarioStats {
    size_t tx_count = 0;
    size_t rx_count = 0;
    size_t signals_generated = 0;
    size_t signals_intended = 0;
    std::atomic<size_t> signals_received{0};
    std::atomic<size_t> signals_dropped_old{0};
    std::atomic<size_t> signals_dropped_pool{0};
    std::atomic<size_t> signals_dropped_age{0};
    std::atomic<bool> measuring{false};

    // Signal-level latency breakdown (computed from recorders)
    LatencyResult recv_to_proc_start;
    LatencyResult proc_start_to_end;
    LatencyResult proc_end_to_send;
    LatencyResult total_latency;

    // Channel impairment step latencies
    LatencyResult latency_resample;
    LatencyResult latency_cir;
    LatencyResult latency_pathloss;
    LatencyResult latency_noise;
    LatencyResult latency_freq_offset;

    double cpu_user_time_ms = 0;
    double cpu_system_time_ms = 0;
    size_t peak_memory_kb = 0;

    steady_clock::time_point start_time;
    steady_clock::time_point end_time;

    ScenarioStats() = default;
    ScenarioStats(const ScenarioStats& o)
        : tx_count(o.tx_count),
          rx_count(o.rx_count),
          signals_generated(o.signals_generated),
          signals_intended(o.signals_intended),
          signals_received(o.signals_received.load()),
          signals_dropped_old(o.signals_dropped_old.load()),
          signals_dropped_pool(o.signals_dropped_pool.load()),
          signals_dropped_age(o.signals_dropped_age.load()),
          recv_to_proc_start(o.recv_to_proc_start),
          proc_start_to_end(o.proc_start_to_end),
          proc_end_to_send(o.proc_end_to_send),
          total_latency(o.total_latency),
          latency_resample(o.latency_resample),
          latency_cir(o.latency_cir),
          latency_pathloss(o.latency_pathloss),
          latency_noise(o.latency_noise),
          latency_freq_offset(o.latency_freq_offset),
          cpu_user_time_ms(o.cpu_user_time_ms),
          cpu_system_time_ms(o.cpu_system_time_ms),
          peak_memory_kb(o.peak_memory_kb),
          start_time(o.start_time),
          end_time(o.end_time) {}

    void reset() {
        signals_generated = 0;
        signals_intended = 0;
        signals_received = 0;
        signals_dropped_old = 0;
        signals_dropped_pool = 0;
        signals_dropped_age = 0;
        cpu_user_time_ms = 0;
        cpu_system_time_ms = 0;
        peak_memory_kb = 0;
    }

    size_t expected() const { return signals_generated * tx_count; }

    size_t pool_exhausted() const {
        return signals_intended - signals_generated;
    }

    size_t unaccounted_drops() const {
        auto exp = expected();
        auto recv = signals_received.load();
        auto old = signals_dropped_old.load();
        auto pool = signals_dropped_pool.load();
        auto age = signals_dropped_age.load();
        auto known_drops = old + pool + age * tx_count;
        if (recv + known_drops >= exp) return 0;
        return exp - recv - known_drops;
    }

    double drop_rate() const {
        auto exp = expected();
        return exp == 0 ? 0.0 : 1.0 - double(signals_received.load()) / exp;
    }

    double throughput() const {
        auto ms = duration_cast<milliseconds>(end_time - start_time).count();
        return ms == 0 ? 0.0 : double(signals_received.load()) / ms * 1000.0;
    }
};

void BenchmarkRecorders::computeAll(ScenarioStats& stats) {
    stats.recv_to_proc_start = recv_to_proc_start.compute();
    stats.proc_start_to_end = proc_start_to_end.compute();
    stats.proc_end_to_send = proc_end_to_send.compute();
    stats.total_latency = total_latency.compute();
    stats.latency_resample = latency_resample.compute();
    stats.latency_cir = latency_cir.compute();
    stats.latency_pathloss = latency_pathloss.compute();
    stats.latency_noise = latency_noise.compute();
    stats.latency_freq_offset = latency_freq_offset.compute();
}

// Collects processed signals instead of sending via network
class MockTransmitter : public chem::emulator::Transmitter {
   public:
    MockTransmitter(boost::asio::io_service& io, const chem::NodeConfig& config,
                    iqPoolSPtr iq_pool, ScenarioStats& stats,
                    BenchmarkRecorders& recorders, int64_t max_latency_ms)
        : Transmitter(io, config, iq_pool),
          m_stats(stats),
          m_recorders(recorders),
          m_pool(iq_pool),
          m_max_latency_ns(MS_2_NS(max_latency_ms)) {}

    void Add2Buff(std::unique_ptr<chem::Signal> sig) override {
        if (!m_stats.measuring.load(std::memory_order_acquire)) {
            m_pool->release(sig->dataArray());
            return;
        }

        auto now_ns =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count();

        if (sig->getHeader()->start < (now_ns - m_max_latency_ns)) {
            m_stats.signals_dropped_old.fetch_add(1, std::memory_order_relaxed);
            m_pool->release(sig->dataArray());
            return;
        }

        // Record per-output-signal latencies
        int64_t t_recv = sig->getTRecv();
        int64_t t_ps = sig->getTProcStart();
        int64_t t_pe = sig->getTProcEnd();

        if (t_recv > 0 && t_ps > 0)
            m_recorders.recv_to_proc_start.record(t_ps - t_recv);
        if (t_pe > 0) m_recorders.proc_end_to_send.record(now_ns - t_pe);
        if (t_recv > 0) m_recorders.total_latency.record(now_ns - t_recv);

        m_stats.signals_received.fetch_add(1, std::memory_order_relaxed);
        m_pool->release(sig->dataArray());
    }

   private:
    ScenarioStats& m_stats;
    BenchmarkRecorders& m_recorders;
    iqPoolSPtr m_pool;
    int64_t m_max_latency_ns;
};

class SignalGenerator {
   public:
    SignalGenerator(const BenchmarkConfig& cfg, iqPoolSPtr pool)
        : m_pool(pool),
          m_duration_ns(static_cast<int64_t>(cfg.signal_duration_us * 1000)) {
        size_t samples =
            static_cast<size_t>(cfg.signal_duration_us * cfg.sample_rate / 1e6);
        m_hdr = {};
        m_hdr.size = computeTotalBytes(cfg);
        m_hdr.sample_rate = cfg.sample_rate;
        m_hdr.number_of_samples = samples;
        m_hdr.total_frags = 1;
        m_hdr.number_of_channels = cfg.num_channels;
    }

    std::unique_ptr<chem::Signal> generate(const std::string& src_id,
                                           double freq) {
        if (m_pool->available() == 0) return nullptr;

        std::unique_ptr<char[]> buffer;
        try {
            buffer = m_pool->acquire();
        } catch (const std::runtime_error&) {
            return nullptr;
        }

        return std::make_unique<chem::Signal>(std::move(buffer), m_hdr, freq,
                                              src_id);
    }

    void stampSignal(chem::Signal& sig, int64_t now_ns) {
        auto* hdr = sig.getHeader();
        hdr->start = now_ns;
        hdr->end = now_ns + m_duration_ns;
        hdr->rt_tx_time = now_ns;
    }

   private:
    static size_t computeTotalBytes(const BenchmarkConfig& cfg) {
        size_t samples =
            static_cast<size_t>(cfg.signal_duration_us * cfg.sample_rate / 1e6);
        if (samples == 0) throw std::runtime_error("samples per signal is 0");
        if (samples > MAX_NUM_SAMPLES)
            throw std::runtime_error("samples (" + std::to_string(samples) +
                                     ") exceeds MAX_NUM_SAMPLES (" +
                                     std::to_string(MAX_NUM_SAMPLES) + ")");
        size_t bytes = samples * 4 * cfg.num_channels;
        if (bytes > IQ_SIZE)
            throw std::runtime_error("signal size (" + std::to_string(bytes) +
                                     ") exceeds IQ_SIZE (" +
                                     std::to_string(IQ_SIZE) + ")");
        return bytes;
    }

    iqPoolSPtr m_pool;
    int64_t m_duration_ns;
    struct Header m_hdr;
};

class BenchmarkRunner {
   public:
    explicit BenchmarkRunner(const BenchmarkConfig& config)
        : m_config(config),
          m_payload_pool(std::make_shared<udpDataPool_t>(
              POOL_CAP * 16, "benchmark_payload")) {}

    static constexpr size_t BENCH_QUEUE_DEPTH = 64;

    static size_t poolSize(size_t tx_count, size_t rx_count) {
        size_t queue_depth =
            std::min(static_cast<size_t>(QUEUE_CAP), BENCH_QUEUE_DEPTH);
        size_t need = rx_count * (queue_depth + tx_count) + rx_count * 32;
        return need;
    }

    static size_t signalBufferSize(const BenchmarkConfig& cfg) {
        size_t samples =
            static_cast<size_t>(cfg.signal_duration_us * cfg.sample_rate / 1e6);
        size_t bytes = samples * 4 * cfg.num_channels;
        return bytes + HEADER_SIZE;  // safety margin
    }

    bool runAllScenarios() {
        printConfig();
        return m_config.max_mode ? runMaxMode() : runFullMode();
    }

   private:
    bool runFullMode() {
        bool all_passed = true;
        for (size_t n = 1; n <= m_config.max_connections; n++) {
            auto median = runWithMedian(n, n);
            bool passed = median.drop_rate() < m_config.drop_threshold;
            m_results.push_back(median);
            printResults(median, passed);
            if (!passed) all_passed = false;
            writeResultsCSV();
            std::this_thread::sleep_for(500ms);
        }

        size_t max_nodes = maxPassedNodes();

        std::cout << "\n========================================\n"
                  << "  BENCHMARK " << (all_passed ? "PASSED" : "FAILED")
                  << "\n========================================\n"
                  << "  Maximum supported nodes: " << max_nodes << "\n"
                  << "  (Based on " << m_config.drop_threshold * 100
                  << "% drop rate threshold)\n\n";
        return all_passed;
    }

    bool runMaxMode() {
        size_t last_passed = 0;
        for (size_t n = 1; n <= m_config.max_connections; n *= 2) {
            auto median = runWithMedian(n, n);
            bool passed = median.drop_rate() < m_config.drop_threshold;
            m_results.push_back(median);
            printResults(median, passed);
            if (!passed) {
                std::cout << "\n========================================\n"
                          << "  BENCHMARK STOPPED (failure)\n"
                          << "========================================\n"
                          << "  Maximum supported nodes: " << last_passed
                          << "\n"
                          << "  Failed at: " << n << " nodes\n\n";
                writeResultsCSV();
                return false;
            }
            last_passed = n;
            std::this_thread::sleep_for(500ms);
        }

        writeResultsCSV();
        std::cout << "\n========================================\n"
                  << "  BENCHMARK PASSED (all scenarios)\n"
                  << "========================================\n"
                  << "  Maximum supported nodes: " << last_passed << "+\n\n";
        return true;
    }

    ScenarioStats runWithMedian(size_t tx_count, size_t rx_count) {
        const size_t R = m_config.runs;
        // Collect drop rates and corresponding indices so we can pick
        // the median without sorting ScenarioStats (non-assignable due
        // to std::atomic members).
        std::vector<ScenarioStats> run_results;
        std::vector<double> drop_rates;
        run_results.reserve(R);
        drop_rates.reserve(R);

        for (size_t r = 0; r < R; r++) {
            if (R > 1)
                std::cout << "\n>>> Run " << (r + 1) << " of " << R
                          << " for " << tx_count << "TX x " << rx_count
                          << "RX\n";

            run_results.emplace_back();
            auto& stats = run_results.back();
            stats.tx_count = tx_count;
            stats.rx_count = rx_count;
            runScenario(tx_count, rx_count, stats);
            drop_rates.push_back(stats.drop_rate());
            if (m_config.verbose && R > 1)
                std::cout << "  Run " << (r + 1)
                          << " drop rate: " << std::fixed
                          << std::setprecision(2)
                          << drop_rates.back() * 100 << "%\n";
            if (r < R - 1)
                std::this_thread::sleep_for(500ms);
        }

        // Sort indices by drop rate, pick the median index
        std::vector<size_t> idx(R);
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(),
                  [&drop_rates](size_t a, size_t b) {
                      return drop_rates[a] < drop_rates[b];
                  });
        size_t median_idx = idx[R / 2];

        if (R > 1 && m_config.verbose)
            std::cout << "  Median drop rate: " << std::fixed
                      << std::setprecision(2)
                      << drop_rates[median_idx] * 100 << "%\n";
        return run_results[median_idx];
    }

    bool runAndRecord(size_t tx_count, size_t rx_count) {
        std::cout << "----------------------------------------\n"
                  << "Running scenario: " << tx_count << "TX x " << rx_count
                  << "RX\n"
                  << "----------------------------------------\n";

        ScenarioStats stats;
        stats.tx_count = tx_count;
        stats.rx_count = rx_count;

        bool passed = runScenario(tx_count, rx_count, stats);
        m_results.push_back(stats);
        printResults(stats, passed);
        return passed;
    }

    bool runScenario(size_t tx_count, size_t rx_count, ScenarioStats& stats) {
        size_t pool_sz = poolSize(tx_count, rx_count);
        size_t buf_sz = signalBufferSize(m_config);
        m_iq_pool = std::make_shared<iqPool_t>(pool_sz, buf_sz, "benchmark_iq");

        double pool_mb =
            static_cast<double>(pool_sz) * buf_sz / (1024.0 * 1024.0);
        std::cout << "  IQ pool: " << pool_sz << " buffers x " << buf_sz
                  << " B (" << std::fixed << std::setprecision(1) << pool_mb
                  << " MB)\n"
                  << "  Injection window:  " << m_config.signal_duration_us
                  << " us (1 frame/rx, randomized)\n";

        // Create recorders sized for expected throughput
        BenchmarkRecorders recorders;
        size_t expected_signals = static_cast<size_t>(
            m_config.benchmark_duration_s * 1e6 /
            m_config.signal_duration_us * rx_count * tx_count);
        recorders.reserve(expected_signals + expected_signals / 10);

        auto intermediate = std::make_unique<chem::Intermediate>(
            m_config.frequency_hz, m_payload_pool,
            std::vector<iqPoolSPtr>{m_iq_pool}, m_config.max_latency_ms);

        if (m_config.detailed_timing)
            installTimingCallback(*intermediate, recorders, stats);

        intermediate->setSignalProcessingCallback(
            [&recorders, &stats](int64_t duration_ns) {
                if (!stats.measuring.load(std::memory_order_acquire)) return;
                recorders.proc_start_to_end.record(duration_ns);
            });

        auto transmitters = createTransmitters(tx_count, stats, recorders);
        for (auto& tx : transmitters) intermediate->AttachDestination(tx);

        std::vector<chem::NodeConfig> rx_configs;
        auto receivers = createReceivers(rx_count, rx_configs);
        for (auto& rx : receivers) intermediate->AttachSource(rx);

        if (m_config.cir_taps > 0) {
            std::mt19937 rng(42);
            std::normal_distribution<float> dist(0.0f, 1.0f);
            for (auto& [key, channel] : intermediate->getChannelList()) {
                chem::signal_v taps(m_config.cir_taps);
                float mag = 1.0f;
                for (size_t t = 0; t < m_config.cir_taps; t++) {
                    taps[t] = chem::fc(dist(rng) * mag, dist(rng) * mag);
                    mag *= 0.7f;
                }
                float power = 0.0f;
                for (const auto& tap : taps) power += std::norm(tap);
                if (power > 0.0f) {
                    float scale = 1.0f / std::sqrt(power);
                    for (auto& tap : taps) tap *= scale;
                }
                channel.updateChTaps({0, 0}, taps);
                channel.setCIRApplyMethod(
                    chem::dsp::channel::CIRApplyMethod::FIR_CAUSAL);
            }
            if (m_config.verbose)
                std::cout << "  CIR taps configured: " << m_config.cir_taps
                          << " per channel\n";
        }

        std::this_thread::sleep_for(100ms);

        struct rusage ru_start;
        getrusage(RUSAGE_SELF, &ru_start);

        SignalGenerator gen(m_config, m_iq_pool);
        // Warmup
        if (m_config.verbose)
            std::cout << "  Warmup phase (" << m_config.warmup_duration_s
                      << "s)...\n";
        injectSignals(gen, receivers, rx_configs,
                      m_config.warmup_duration_s, nullptr);

        // Wait for worker queues to fully drain before starting measurement.
        // Use >= because pool.unsafe_size() is approximate under concurrent
        // access and may never report exactly pool_sz.
        for (int i = 0; i < 50; i++) {
            std::this_thread::sleep_for(100ms);
            if (m_iq_pool->available() >= pool_sz) break;
        }
        // Grace period: ensure all in-flight signals are fully delivered
        std::this_thread::sleep_for(50ms);

        stats.reset();
        recorders.reset();
        intermediate->resetDropCounters();
        stats.measuring.store(true, std::memory_order_release);

        if (m_config.verbose)
            std::cout << "  Measurement phase ("
                      << m_config.benchmark_duration_s << "s)...\n";
        stats.start_time = steady_clock::now();
        injectSignals(gen, receivers, rx_configs,
                      m_config.benchmark_duration_s, &stats);

        // Cooldown: poll until all buffers returned (all signals fully
        // processed) with 5s timeout.  Keep measuring=true so that signals
        // arriving at MockTransmitter during drain are still counted.
        if (m_config.verbose)
            std::cout << "  Cooldown drain...\n";
        auto drain_deadline = steady_clock::now() + 5s;
        while (steady_clock::now() < drain_deadline) {
            if (m_iq_pool->available() >= pool_sz) break;
            std::this_thread::sleep_for(10ms);
        }
        // Grace period: let any final in-flight signals reach MockTransmitter
        // before disabling measurement.  This closes the race where a signal
        // finishes processing between the pool check and measuring=false.
        std::this_thread::sleep_for(50ms);

        stats.measuring.store(false, std::memory_order_release);
        stats.end_time = steady_clock::now();

        // Capture drop counters from Intermediate
        stats.signals_dropped_pool.store(
            intermediate->getPoolDropCount(), std::memory_order_relaxed);
        stats.signals_dropped_age.store(
            intermediate->getWorkerAgeDropCount(),
            std::memory_order_relaxed);

        // Compute percentiles from raw samples
        recorders.computeAll(stats);

        struct rusage ru_end;
        getrusage(RUSAGE_SELF, &ru_end);
        stats.cpu_user_time_ms =
            (ru_end.ru_utime.tv_sec - ru_start.ru_utime.tv_sec) * 1000.0 +
            (ru_end.ru_utime.tv_usec - ru_start.ru_utime.tv_usec) / 1000.0;
        stats.cpu_system_time_ms =
            (ru_end.ru_stime.tv_sec - ru_start.ru_stime.tv_sec) * 1000.0 +
            (ru_end.ru_stime.tv_usec - ru_start.ru_stime.tv_usec) / 1000.0;
        stats.peak_memory_kb = ru_end.ru_maxrss;

        for (auto& rx : receivers) intermediate->DetachSource(rx);
        for (auto& tx : transmitters) intermediate->DetachDestination(tx);

        return stats.drop_rate() < m_config.drop_threshold;
    }

    void injectSignals(
        SignalGenerator& gen,
        const std::vector<std::shared_ptr<chem::emulator::Receiver>>& receivers,
        const std::vector<chem::NodeConfig>& rx_configs,
        double duration_s, ScenarioStats* stats) {
        const size_t n_rx = receivers.size();
        const int64_t window_ns =
            static_cast<int64_t>(m_config.signal_duration_us) * 1000;
        const size_t total_windows =
            static_cast<size_t>(duration_s * 1e6 /
                                m_config.signal_duration_us);

        // Fixed seed for reproducibility
        std::mt19937 rng(42);
        std::uniform_int_distribution<int64_t> dist(0, window_ns - 1);

        // Pre-allocate schedule: {offset_ns, receiver_index}
        std::vector<std::pair<int64_t, size_t>> schedule(n_rx);

        for (size_t w = 0; w < total_windows; w++) {
            auto window_start = steady_clock::now();

            // Generate random offsets for each receiver within this window
            for (size_t i = 0; i < n_rx; i++)
                schedule[i] = {dist(rng), i};
            std::sort(schedule.begin(), schedule.end());

            for (const auto& [offset_ns, rx_idx] : schedule) {
                // Spin-wait until this injection's target time
                auto target = window_start + nanoseconds(offset_ns);
                while (steady_clock::now() < target) { /* spin */ }

                if (stats) stats->signals_intended++;

                auto sig = gen.generate(rx_configs[rx_idx].getId(),
                                        m_config.frequency_hz);
                if (!sig) continue;

                if (stats) stats->signals_generated++;

                int64_t now_ns = duration_cast<nanoseconds>(
                                     steady_clock::now().time_since_epoch())
                                     .count();
                gen.stampSignal(*sig, now_ns);
                sig->setTRecv(now_ns);
                receivers[rx_idx]->Add2Buff(std::move(sig));
            }

            // Spin-wait until the window boundary
            auto window_end = window_start + nanoseconds(window_ns);
            while (steady_clock::now() < window_end) { /* spin */ }
        }
    }

    chem::NodeConfig makeNodeConfig(const std::string& id,
                                    const std::string& name, short tx_port,
                                    short rx_port) {
        chem::NodeConfig cfg;
        cfg.setId(id);
        cfg.setName(name);
        cfg.setNumChannels(m_config.num_channels);
        cfg.setIpAddress("127.0.0.1");
        cfg.setTxPortNumber(tx_port);
        cfg.setRxPortNumber(rx_port);

        chem::usrp::SampleRate sr;
        sr.setRxRate(m_config.sample_rate);
        sr.setTxRate(m_config.sample_rate);
        cfg.setSampleRate(sr);

        chem::usrp::Channel ch;
        ch.setRxFreq(m_config.frequency_hz);
        ch.setTxFreq(m_config.frequency_hz);
        cfg.addChannel(ch);
        cfg.setAntennaPattern("dipole");
        return cfg;
    }

    std::vector<std::shared_ptr<MockTransmitter>> createTransmitters(
        size_t count, ScenarioStats& stats,
        BenchmarkRecorders& recorders) {
        std::vector<std::shared_ptr<MockTransmitter>> txs;
        txs.reserve(count);
        for (size_t i = 0; i < count; i++) {
            auto cfg = makeNodeConfig("tx_" + std::to_string(i),
                                      "TX-" + std::to_string(i),
                                      30000 + short(i), 20000 + short(i));
            txs.push_back(std::make_shared<MockTransmitter>(
                m_io_service, cfg, m_iq_pool, stats, recorders,
                m_config.max_latency_ms));
        }
        return txs;
    }

    std::vector<std::shared_ptr<chem::emulator::Receiver>> createReceivers(
        size_t count, std::vector<chem::NodeConfig>& configs) {
        std::vector<std::shared_ptr<chem::emulator::Receiver>> rxs;
        rxs.reserve(count);
        configs.reserve(count);
        for (size_t i = 0; i < count; i++) {
            auto cfg = makeNodeConfig("rx_" + std::to_string(i),
                                      "RX-" + std::to_string(i),
                                      10000 + short(i), 40000 + short(i));
            configs.push_back(cfg);
            rxs.push_back(std::make_shared<chem::emulator::Receiver>(
                m_io_service, cfg, m_iq_pool));
        }
        return rxs;
    }

    void installTimingCallback(chem::Intermediate& intermediate,
                               BenchmarkRecorders& recorders,
                               ScenarioStats& stats) {
        intermediate.setTimingCallback(
            [&recorders, &stats](const chem::ChannelProcessTiming& t) {
                if (!stats.measuring.load(std::memory_order_acquire)) return;
                if (t.t_resample > t.t_start)
                    recorders.latency_resample.record(t.t_resample -
                                                      t.t_start);
                if (t.t_cir > t.t_resample)
                    recorders.latency_cir.record(t.t_cir - t.t_resample);
                if (t.t_pathloss > t.t_cir)
                    recorders.latency_pathloss.record(t.t_pathloss - t.t_cir);
                if (t.t_noise > t.t_pathloss)
                    recorders.latency_noise.record(t.t_noise - t.t_pathloss);
                if (t.t_freq_offset > t.t_noise)
                    recorders.latency_freq_offset.record(t.t_freq_offset -
                                                         t.t_noise);
            });
    }

    void printConfig() {
        std::cout
            << "\n========================================\n"
            << "      CHEM Benchmark Tool\n"
            << "========================================\n\n"
            << "Configuration:\n"
            << "  Mode:              "
            << (m_config.max_mode ? "Max (stop on first failure)" : "Full")
            << "\n"
            << "  Detailed latency:  "
            << (m_config.detailed_timing ? "Yes" : "No") << "\n"
            << "  Max connections:   " << m_config.max_connections << "\n"
            << "  Duration:          " << m_config.benchmark_duration_s
            << " s\n"
            << "  Warmup:            " << m_config.warmup_duration_s << " s\n"
            << "  Sample rate:       " << m_config.sample_rate / 1e6 << " MHz\n"
            << "  Frequency:         " << m_config.frequency_hz / 1e9
            << " GHz\n"
            << "  Channels/node:     " << int(m_config.num_channels) << "\n"
            << "  Signal duration:   " << m_config.signal_duration_us << " us\n"
            << "  Injection:         1 frame/rx/window (randomized)\n"
            << "  Max latency:       " << m_config.max_latency_ms << " ms\n"
            << "  Runs per scenario: " << m_config.runs << "\n"
            << "  Drop threshold:    " << m_config.drop_threshold * 100 << "%\n"
            << "  IQ pool:           sized per scenario (right-sized buffers)\n"
            << "  Worker threads:    " << m_config.max_connections
            << " (1 per receiver)\n"
            << "  TBB parallelism:   " << m_config.max_connections + 2
            << "\n"
            << "  NUMA node:         "
            << (m_config.numa_node >= 0
                    ? std::to_string(m_config.numa_node)
                    : "not pinned")
            << "\n\n";
    }

    void printResults(const ScenarioStats& s, bool passed) {
        auto ms =
            duration_cast<milliseconds>(s.end_time - s.start_time).count();

        std::cout << "\nResults:\n"
                  << std::fixed << std::setprecision(2)
                  << "  Signals intended:  " << s.signals_intended << "\n"
                  << "  Signals generated: " << s.signals_generated << "\n"
                  << "  Pool exhaustion:   " << s.pool_exhausted() << "\n"
                  << "  Expected outputs:  " << s.expected() << "\n"
                  << "  Signals received:  " << s.signals_received.load()
                  << "\n"
                  << "  Dropped (too old): " << s.signals_dropped_old.load()
                  << "\n"
                  << "  Dropped (age/wkr): " << s.signals_dropped_age.load()
                  << "\n"
                  << "  Dropped (pool):    " << s.signals_dropped_pool.load()
                  << "\n"
                  << "  Unaccounted drops: " << s.unaccounted_drops()
                  << "\n"
                  << "  Drop rate:         " << s.drop_rate() * 100 << "%\n";

        auto p = [](const char* label, const LatencyResult& l) {
            if (l.count == 0) return;
            std::cout << "  " << label << "avg=" << l.avg_us
                      << " p50=" << l.p50_us << " p95=" << l.p95_us
                      << " p99=" << l.p99_us << " p99.9=" << l.p999_us
                      << " max=" << l.max_us << " (n=" << l.count << ")\n";
        };
        std::cout << "\nSignal Latency (us):\n";
        p("Recv->ProcStart: ", s.recv_to_proc_start);
        p("Processing:      ", s.proc_start_to_end);
        p("ProcEnd->Send:   ", s.proc_end_to_send);
        p("Total E2E:       ", s.total_latency);

        if (m_config.detailed_timing) {
            std::cout << "\nChannel Impairment Step Latency (us):\n";
            p("Resample:        ", s.latency_resample);
            p("CIR/Multipath:   ", s.latency_cir);
            p("Path Loss:       ", s.latency_pathloss);
            p("AWGN Noise:      ", s.latency_noise);
            p("Freq Offset:     ", s.latency_freq_offset);
        }

        std::cout << "\nPerformance:\n"
                  << "  Duration:          " << ms << " ms\n"
                  << "  Throughput:        " << std::setprecision(0)
                  << s.throughput() << " signals/sec\n"
                  << "\nResource Usage:\n"
                  << std::setprecision(2)
                  << "  CPU User Time:     " << s.cpu_user_time_ms << " ms\n"
                  << "  CPU System Time:   " << s.cpu_system_time_ms << " ms\n"
                  << "  Peak Memory:       " << s.peak_memory_kb << " KB\n"
                  << "\nStatus: " << (passed ? "PASSED" : "FAILED")
                  << " (threshold: " << m_config.drop_threshold * 100
                  << "% drop rate)\n";
    }

    void writeResultsCSV() {
        std::ofstream f(m_config.output_file);
        if (!f.is_open()) {
            std::cerr << "Warning: Could not open " << m_config.output_file
                      << "\n";
            return;
        }

        auto writeLatencyHeader = [&f](const std::string& prefix) {
            f << prefix << "_avg_us," << prefix << "_min_us," << prefix
              << "_p50_us," << prefix << "_p90_us," << prefix << "_p95_us,"
              << prefix << "_p99_us," << prefix << "_p999_us," << prefix
              << "_max_us," << prefix << "_count,";
        };

        auto writeLatencyData = [&f](const LatencyResult& r) {
            f << r.avg_us << "," << r.min_us << "," << r.p50_us << ","
              << r.p90_us << "," << r.p95_us << "," << r.p99_us << ","
              << r.p999_us << "," << r.max_us << "," << r.count << ",";
        };

        // Header
        f << "tx_count,rx_count,"
          << "signals_generated,signals_intended,signals_expected,"
          << "signals_received,signals_dropped_old,signals_dropped_age,"
          << "signals_dropped_pool,signals_pool_exhausted,"
          << "drop_rate_pct,throughput_signals_per_sec,";

        writeLatencyHeader("latency_recv_proc");
        writeLatencyHeader("latency_proc");
        writeLatencyHeader("latency_proc_send");
        writeLatencyHeader("latency_total");

        if (m_config.detailed_timing) {
            writeLatencyHeader("latency_resample");
            writeLatencyHeader("latency_cir");
            writeLatencyHeader("latency_pathloss");
            writeLatencyHeader("latency_noise");
            writeLatencyHeader("latency_freq_offset");
        }

        f << "duration_ms,cpu_user_ms,cpu_system_ms,peak_memory_kb\n";

        // Data rows
        for (const auto& s : m_results) {
            auto ms =
                duration_cast<milliseconds>(s.end_time - s.start_time).count();

            f << s.tx_count << "," << s.rx_count << "," << s.signals_generated
              << "," << s.signals_intended << "," << s.expected() << ","
              << s.signals_received.load() << ","
              << s.signals_dropped_old.load() << ","
              << s.signals_dropped_age.load() << ","
              << s.signals_dropped_pool.load() << ","
              << s.pool_exhausted()
              << "," << std::fixed << std::setprecision(4)
              << s.drop_rate() * 100 << "," << std::setprecision(2)
              << s.throughput() << ",";

            writeLatencyData(s.recv_to_proc_start);
            writeLatencyData(s.proc_start_to_end);
            writeLatencyData(s.proc_end_to_send);
            writeLatencyData(s.total_latency);

            if (m_config.detailed_timing) {
                writeLatencyData(s.latency_resample);
                writeLatencyData(s.latency_cir);
                writeLatencyData(s.latency_pathloss);
                writeLatencyData(s.latency_noise);
                writeLatencyData(s.latency_freq_offset);
            }

            f << ms << "," << s.cpu_user_time_ms << "," << s.cpu_system_time_ms
              << "," << s.peak_memory_kb << "\n";
        }

        std::cout << "\nResults written to: " << m_config.output_file << "\n";
    }

    size_t maxPassedNodes() {
        size_t max = 0;
        for (const auto& s : m_results)
            if (s.drop_rate() < m_config.drop_threshold)
                max = std::max(max, s.tx_count);
        return max;
    }

    BenchmarkConfig m_config;
    boost::asio::io_service m_io_service;
    payloadPoolSPtr m_payload_pool;
    iqPoolSPtr m_iq_pool;
    std::vector<ScenarioStats> m_results;
};

void printUsage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  -m, --max-connections N    Maximum TX/RX pairs (default: 8)\n"
        << "  -M, --max-mode             Stop at first failure\n"
        << "  -d, --detailed-timing      Collect per-step channel timing\n"
        << "  -s, --duration S           Measurement duration in seconds "
           "(default: 10)\n"
        << "  -C, --cir-taps N           Random CIR taps (default: 0 = "
           "disabled)\n"
        << "  -f, --frequency F          Frequency in Hz (default: 2.68e9)\n"
        << "  -r, --sample-rate R        Sample rate in Hz (default: "
           "11520000)\n"
        << "  -c, --channels N           Channels per node (default: 1)\n"
        << "  -D, --signal-duration US   Signal duration in us (default: "
           "1000)\n"
        << "  -l, --max-latency MS       Max acceptable latency in ms "
           "(default: "
           "5)\n"
        << "  -t, --threshold P          Drop threshold percentage (default: "
           "10)\n"
        << "  -o, --output FILE          Output CSV file\n"
        << "  -R, --runs N               Repeat each scenario N times, report "
           "median (default: 1)\n"
        << "  -N, --numa-node N          Pin to NUMA node N (default: none)\n"
        << "  -v, --verbose              Verbose output\n"
        << "  -h, --help                 Show this help\n";
}

BenchmarkConfig parseArgs(int argc, char* argv[]) {
    BenchmarkConfig cfg;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            exit(0);
        } else if ((arg == "-m" || arg == "--max-connections") &&
                   i + 1 < argc) {
            cfg.max_connections = std::stoul(argv[++i]);
        } else if (arg == "-M" || arg == "--max-mode") {
            cfg.max_mode = true;
        } else if (arg == "-d" || arg == "--detailed-timing") {
            cfg.detailed_timing = true;
        } else if ((arg == "-s" || arg == "--duration") && i + 1 < argc) {
            cfg.benchmark_duration_s = std::stod(argv[++i]);
        } else if ((arg == "-C" || arg == "--cir-taps") && i + 1 < argc) {
            cfg.cir_taps = std::stoul(argv[++i]);
        } else if ((arg == "-f" || arg == "--frequency") && i + 1 < argc) {
            cfg.frequency_hz = std::stod(argv[++i]);
        } else if ((arg == "-r" || arg == "--sample-rate") && i + 1 < argc) {
            cfg.sample_rate = std::stoul(argv[++i]);
        } else if ((arg == "-c" || arg == "--channels") && i + 1 < argc) {
            int ch = std::stoi(argv[++i]);
            if (ch < 1 || ch > N_CH) {
                std::cerr << "Error: channels must be between 1 and " << N_CH
                          << "\n";
                exit(1);
            }
            cfg.num_channels = static_cast<uint8_t>(ch);
        } else if ((arg == "-D" || arg == "--signal-duration") &&
                   i + 1 < argc) {
            cfg.signal_duration_us = std::stoul(argv[++i]);
        } else if ((arg == "-l" || arg == "--max-latency") && i + 1 < argc) {
            cfg.max_latency_ms = std::stoll(argv[++i]);
        } else if ((arg == "-t" || arg == "--threshold") && i + 1 < argc) {
            cfg.drop_threshold = std::stod(argv[++i]) / 100.0;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            cfg.output_file = argv[++i];
        } else if ((arg == "-R" || arg == "--runs") && i + 1 < argc) {
            cfg.runs = std::stoul(argv[++i]);
        } else if ((arg == "-N" || arg == "--numa-node") && i + 1 < argc) {
            cfg.numa_node = std::stoi(argv[++i]);
        } else if (arg == "-v" || arg == "--verbose") {
            cfg.verbose = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            exit(1);
        }
    }

    if (cfg.max_connections == 0) {
        std::cerr << "Error: max-connections must be >= 1\n";
        exit(1);
    }
    if (cfg.benchmark_duration_s <= 0) {
        std::cerr << "Error: duration must be positive\n";
        exit(1);
    }
    if (cfg.drop_threshold < 0 || cfg.drop_threshold > 1) {
        std::cerr << "Error: threshold 0-100\n";
        exit(1);
    }
    if (cfg.max_latency_ms <= 0) {
        std::cerr << "Error: max-latency must be positive\n";
        exit(1);
    }
    if (cfg.signal_duration_us == 0) {
        std::cerr << "Error: signal-duration must be >= 1\n";
        exit(1);
    }
    if (cfg.runs == 0) {
        std::cerr << "Error: runs must be >= 1\n";
        exit(1);
    }

    auto samples = static_cast<long double>(cfg.signal_duration_us) *
                   static_cast<long double>(cfg.sample_rate) / 1e6L;
    auto bytes = samples * 4.0L * static_cast<long double>(cfg.num_channels);

    if (bytes > static_cast<long double>(IQ_SIZE)) {
        std::cerr << "Error: signal size exceeds IQ buffer. "
                     "Reduce --signal-duration, --sample-rate, or --channels\n";
        exit(1);
    }
    if (samples > static_cast<long double>(MAX_NUM_SAMPLES)) {
        std::cerr << "Error: samples per signal (" << size_t(samples)
                  << ") exceeds MAX_NUM_SAMPLES (" << MAX_NUM_SAMPLES << ")\n";
        exit(1);
    }

    return cfg;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto logger = spdlog::stdout_color_mt("CHEM");
    logger->set_level(spdlog::level::warn);

    auto config = parseArgs(argc, argv);

    if (config.numa_node >= 0) {
        if (chem::numa::available()) {
            chem::numa::bind_process_to_node(config.numa_node);
        } else {
            std::cerr << "Warning: NUMA requested but libnuma not available\n";
        }
    }

    int tbb_threads =
        static_cast<int>(config.max_connections) + 2;  // workers + margin
    tbb::global_control tbb_guard(
        tbb::global_control::max_allowed_parallelism, tbb_threads);

    try {
        BenchmarkRunner runner(config);
        return runner.runAllScenarios() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 2;
    }
}
