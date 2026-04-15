/**
 * @file profiling.h
 * @brief Profiling utility for CHEM performance profiling
 * @author Anıl Gürses <agurses@ncsu.edu>
 * @version v1.0
 *
 * Enable with -DENABLE_PROFILING cmake flag.
 * Outputs CSV to execution directory.
 */

#pragma once

#ifdef ENABLE_PROFILING

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <vector>

namespace chem {
namespace profiling {

using clock_t = std::chrono::steady_clock;
using time_point_t = clock_t::time_point;
using duration_ns = std::chrono::nanoseconds;

// Profiling entry for a single signal processing pass
struct ProfilingEntry {
    // Metadata
    uint64_t timestamp_ns;  // Wall clock timestamp
    std::string src_id;     // Source node ID
    std::string dest_id;    // Destination node ID
    size_t num_samples;     // Number of samples processed
    size_t src_channels;    // Source channels
    size_t dest_channels;   // Destination channels
    float sample_rate_hz;   // Sample rate
    double freq_mhz;        // Channel frequency

    // Timing measurements (nanoseconds)
    int64_t t_conversion_rx;    // RX format conversion time
    int64_t t_lock_acquire;     // Lock acquisition time
    int64_t t_dest_snapshot;    // Destination snapshot time
    int64_t t_buffer_clear;     // Buffer clearing time
    int64_t t_pool_acquire;     // Memory pool acquisition
    int64_t t_channel_process;  // Channel DSP processing (total)
    int64_t t_conversion_tx;    // TX format conversion time
    int64_t t_signal_create;    // Signal creation time
    int64_t t_queue_add;        // Queue addition time
    int64_t t_total;            // Total processing time

    // Detailed channel impairment timing (nanoseconds)
    int64_t t_resample;     // Resampling/gain scaling
    int64_t t_cir;          // CIR/multipath application
    int64_t t_pathloss;     // Path loss calculation
    int64_t t_noise;        // AWGN noise addition
    int64_t t_freq_offset;  // Frequency offset/Doppler
};

// Scoped timer
class ScopedTimer {
   public:
    explicit ScopedTimer(int64_t& target)
        : m_target(target), m_start(clock_t::now()) {}

    ~ScopedTimer() {
        auto end = clock_t::now();
        m_target =
            std::chrono::duration_cast<duration_ns>(end - m_start).count();
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

   private:
    int64_t& m_target;
    time_point_t m_start;
};

class ManualTimer {
   public:
    void start() { m_start = clock_t::now(); }

    int64_t stop() {
        auto end = clock_t::now();
        return std::chrono::duration_cast<duration_ns>(end - m_start).count();
    }

    int64_t elapsed() const {
        auto now = clock_t::now();
        return std::chrono::duration_cast<duration_ns>(now - m_start).count();
    }

   private:
    time_point_t m_start;
};

class ProfilingRecorder {
   public:
    static ProfilingRecorder& instance() {
        static ProfilingRecorder inst;
        return inst;
    }

    void setOutputPath(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputPath = path;
    }

    void record(const ProfilingEntry& entry) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back(entry);

        // Flush every N entries to avoid memory buildup
        if (m_entries.size() >= m_flushThreshold) {
            flushInternal();
        }
    }

    void flush() {
        std::lock_guard<std::mutex> lock(m_mutex);
        flushInternal();
    }

    void setFlushThreshold(size_t threshold) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_flushThreshold = threshold;
    }

    ~ProfilingRecorder() { flush(); }

   private:
    ProfilingRecorder()
        : m_outputPath("chem_profiling.csv"),
          m_flushThreshold(1000),
          m_headerWritten(false) {}

    void flushInternal() {
        if (m_entries.empty()) return;

        std::ofstream file;
        if (!m_headerWritten) {
            file.open(m_outputPath, std::ios::out | std::ios::trunc);
            writeHeader(file);
            m_headerWritten = true;
        } else {
            file.open(m_outputPath, std::ios::out | std::ios::app);
        }

        if (!file.is_open()) {
            return;
        }

        for (const auto& e : m_entries) {
            file << e.timestamp_ns << "," << e.src_id << "," << e.dest_id << ","
                 << e.num_samples << "," << e.src_channels << ","
                 << e.dest_channels << "," << std::fixed << std::setprecision(0)
                 << e.sample_rate_hz << "," << std::fixed
                 << std::setprecision(3) << e.freq_mhz << ","
                 << e.t_conversion_rx << "," << e.t_lock_acquire << ","
                 << e.t_dest_snapshot << "," << e.t_buffer_clear << ","
                 << e.t_pool_acquire << "," << e.t_channel_process << ","
                 << e.t_conversion_tx << "," << e.t_signal_create << ","
                 << e.t_queue_add << "," << e.t_total << "," << e.t_resample
                 << "," << e.t_cir << "," << e.t_pathloss << "," << e.t_noise
                 << "," << e.t_freq_offset << "\n";
        }

        file.close();
        m_entries.clear();
    }

    void writeHeader(std::ofstream& file) {
        file << "timestamp_ns,"
             << "src_id,"
             << "dest_id,"
             << "num_samples,"
             << "src_channels,"
             << "dest_channels,"
             << "sample_rate_hz,"
             << "freq_mhz,"
             << "t_conversion_rx_ns,"
             << "t_lock_acquire_ns,"
             << "t_dest_snapshot_ns,"
             << "t_buffer_clear_ns,"
             << "t_pool_acquire_ns,"
             << "t_channel_process_ns,"
             << "t_conversion_tx_ns,"
             << "t_signal_create_ns,"
             << "t_queue_add_ns,"
             << "t_total_ns,"
             << "t_resample_ns,"
             << "t_cir_ns,"
             << "t_pathloss_ns,"
             << "t_noise_ns,"
             << "t_freq_offset_ns\n";
    }

    std::mutex m_mutex;
    std::vector<ProfilingEntry> m_entries;
    std::string m_outputPath;
    size_t m_flushThreshold;
    bool m_headerWritten;
};

// Convenience macros for profiling
#define PROF_TIMER_START(name)                       \
    chem::profiling::ManualTimer _prof_timer_##name; \
    _prof_timer_##name.start()

#define PROF_TIMER_STOP(name, target) target = _prof_timer_##name.stop()

#define PROF_SCOPED_TIMER(target) \
    chem::profiling::ScopedTimer _scoped_timer_##__LINE__(target)

#define PROF_RECORD(entry) \
    chem::profiling::ProfilingRecorder::instance().record(entry)

#define PROF_FLUSH() chem::profiling::ProfilingRecorder::instance().flush()

#define PROF_SET_OUTPUT(path) \
    chem::profiling::ProfilingRecorder::instance().setOutputPath(path)

}  // namespace profiling
}  // namespace chem

#else  // !ENABLE_PROFILING

// No-op macros
#define PROF_TIMER_START(name) ((void)0)
#define PROF_TIMER_STOP(name, target) ((void)0)
#define PROF_SCOPED_TIMER(target) ((void)0)
#define PROF_RECORD(entry) ((void)0)
#define PROF_FLUSH() ((void)0)
#define PROF_SET_OUTPUT(path) ((void)0)

#endif  // ENABLE_PROFILING
