#include "chem/channel/intermediate.h"

#include <math.h>
#include <spdlog/fmt/fmt.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/io_service.hpp>
#include <chrono>
#include <cmath>
#include <future>
#include <queue>
#include <thread>
#include <utility>

#include "chem/common.h"
#include "chem/models/data_pool.hpp"
#include "chem/numa_utils.h"
#include "chem/profiling.h"

using namespace chem;
using namespace chem::emulator;
using namespace boost::asio;
using namespace std::chrono;
using namespace std::chrono_literals;

Intermediate::Intermediate(const double& freq, payloadPoolSPtr pool,
                           std::vector<iqPoolSPtr> iq_pools,
                           int64_t max_latency_ms)
    : m_iq_pools(std::move(iq_pools)),
      m_freq(freq),
      m_max_latency_ns(MS_2_NS(static_cast<float>(max_latency_ms))),
      m_propagationModel(PropagationModel::FREE_SPACE) {
    LOG_INFO("INTERMEDIATE",
             fmt::format("Channel started on {} MHz!", HZ_TO_MHZ(freq)));
}

Intermediate::~Intermediate() {
    LOG_INFO("INTERMEDIATE",
             fmt::format("Channel is being destroyed on {} MHz!",
                         HZ_TO_MHZ(m_freq)));

    // Signal shutdown and wake up Start() thread
    {
        std::lock_guard<std::mutex> lock(src_mutex);
        work.store(false, std::memory_order_release);
    }
    cv.notify_all();

    // Stop all worker threads
    {
        std::lock_guard<std::mutex> guard(m_workers_mutex);
        for (auto& [id, ctx] : m_workers) {
            ctx->queue.abort();
            ctx->running = false;
        }
    }

    // Wait for all workers to finish
    {
        std::lock_guard<std::mutex> guard(m_workers_mutex);
        for (auto& [id, ctx] : m_workers) {
            if (ctx->worker_thread.joinable()) {
                ctx->worker_thread.join();
            }
        }
        m_workers.clear();
    }
}

// ----------------------------------------------------------------------------
// Worker Management
// ----------------------------------------------------------------------------

void Intermediate::InitWorkerConverters(WorkerContext& ctx) {
    uhd::convert::id_type id_rx;
    id_rx.input_format = "sc16_item32_le";
    id_rx.num_inputs = 1;
    id_rx.output_format = "fc32";
    id_rx.num_outputs = 1;
    ctx.converter_rx = uhd::convert::get_converter(id_rx)();
    ctx.converter_rx->set_scalar(CONV_SCALAR_I);
}

void Intermediate::WorkerLoop(WorkerContext& ctx) {
    // Determine NUMA node for this worker thread
    if (chem::numa::available()) {
        ctx.numa_node = chem::numa::current_node();
    }
    ctx.numa_node =
        std::clamp(ctx.numa_node, 0, static_cast<int>(m_iq_pools.size()) - 1);

    LOG_INFO("INTERMEDIATE",
             fmt::format("Worker thread started for receiver {} (NUMA node {})",
                         ctx.receiver_id, ctx.numa_node));

    while (ctx.running && work) {
        // Wait for destinations
        if (m_destCount.load(std::memory_order_relaxed) == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        std::unique_ptr<chem::Signal> sig;
        try {
            ctx.queue.pop(sig);
        } catch (const tbb::user_abort&) {
            break;
        }

        auto now = steady_clock::now();
        auto now_cnt =
            duration_cast<nanoseconds>(now.time_since_epoch()).count();

        if (sig->getHeader()->start < (now_cnt - m_max_latency_ns)) {
#ifdef ENABLE_DEBUG_LOG
            LOG_WARN("INTERMEDIATE",
                     fmt::format("Worker {}: Signal too old! Diff: {:.3f} ms",
                                 ctx.receiver_id,
                                 NS_2_MS(static_cast<float>(
                                     now_cnt - sig->getHeader()->start))));
#endif
            m_workerAgeDropCount.fetch_add(1, std::memory_order_relaxed);
            m_iq_pools[ctx.numa_node]->release(sig->dataArray());
            continue;
        }

#if defined(ENABLE_DEBUG_LOG) || defined(ENABLE_BENCHMARK)
        auto p_start = steady_clock::now();
        int64_t p_start_cnt =
            duration_cast<nanoseconds>(p_start.time_since_epoch()).count();
        sig->setTProcStart(p_start_cnt);
#endif

        ProcessSignal(ctx, std::move(sig));

#if defined(ENABLE_BENCHMARK)
        if (m_signalProcessingEnabled.load(std::memory_order_acquire)) {
            auto p_end = steady_clock::now();
            m_signalProcessingCallback(
                duration_cast<nanoseconds>(p_end - p_start).count());
        }
#endif
    }

    LOG_INFO(
        "INTERMEDIATE",
        fmt::format("Worker thread stopped for receiver {}", ctx.receiver_id));
}

void Intermediate::Start() {
    // Workers handle all processing - this function just waits for shutdown
    // signal
    LOG_INFO("INTERMEDIATE",
             fmt::format(
                 "Intermediate started on {} MHz - workers handle processing",
                 HZ_TO_MHZ(m_freq)));

    std::unique_lock<std::mutex> lock(src_mutex);
    cv.wait(lock, [this]() { return !work; });
}

// ----------------------------------------------------------------------------
// Core Processing Logic
// ----------------------------------------------------------------------------

void Intermediate::ProcessSignal(WorkerContext& ctx,
                                 std::unique_ptr<chem::Signal> sig) {
    if (!sig) [[unlikely]] {
        LOG_ERROR("INTERMEDIATE", "Signal is empty!");
        return;
    }
    if (!ctx.converter_rx) [[unlikely]] {
        LOG_ERROR("INTERMEDIATE", "Worker converter is not initialized!");
        m_iq_pools[ctx.numa_node]->release(sig->dataArray());
        return;
    }

    SignalContext sig_ctx{};
    sig_ctx.src_n_ch = sig->getNumberOfChannels();
    sig_ctx.i_sz_ch = sig->getSizePerBuff();
    sig_ctx.i_ns_ch = sig->getNumSampPerBuff();
    sig_ctx.sig_srate = sig->getSrate();
    sig_ctx.sig_hdr = *(sig->getHeader());
    sig_ctx.src_id = &ctx.src_node_id;

#if defined(ENABLE_DEBUG_LOG) || defined(ENABLE_BENCHMARK)
    sig_ctx.sig_t_recv = sig->getTRecv();
    sig_ctx.sig_t_proc_start = sig->getTProcStart();
#endif

    ctx.ensureSrcChannels(sig_ctx.src_n_ch);

    PROF_TIMER_START(conv_rx);
    for (size_t ch = 0; ch < sig_ctx.src_n_ch; ch++) {
        auto out_ptr = reinterpret_cast<void*>(ctx.src_sig_arr[ch].data());
        auto in_ptr = reinterpret_cast<const void*>(sig->getData(ch));
        ctx.converter_rx->conv(in_ptr, out_ptr, sig_ctx.i_ns_ch);
    }
    PROF_TIMER_STOP(conv_rx, sig_ctx.t_conversion_rx);

    m_iq_pools[ctx.numa_node]->release(sig->dataArray());

    ChannelProcessParams params{};
    params.propagationModel = m_propagationModel;
    params.ground_coeff = grReflection;
    params.src_config = ctx.src_config;
    params.freq = m_freq;

    auto& destinations = ctx.destinations;

    uint64_t current_version = m_destVersion.load(std::memory_order_acquire);
    if (ctx.cached_dest_version == current_version && current_version != 0) {
        // Reuse cached snapshot — no lock needed
        destinations = ctx.cached_destinations;
        PROF_TIMER_START(lock_acq);
        PROF_TIMER_STOP(lock_acq, sig_ctx.t_lock_acquire);
        PROF_TIMER_START(dest_snap);
        PROF_TIMER_STOP(dest_snap, sig_ctx.t_dest_snapshot);
    } else {
        destinations.clear();
        PROF_TIMER_START(lock_acq);
        {
            std::shared_lock<std::shared_mutex> guard_tx(dest_mutex);
            PROF_TIMER_STOP(lock_acq, sig_ctx.t_lock_acquire);
            PROF_TIMER_START(dest_snap);
            destinations.reserve(m_destinationMap.size());
            for (auto& dest_entry : m_destinationMap) {
                const std::string& dest_id =
                    dest_entry.second->getNodeConfig().getId();
                if (dest_id == *sig_ctx.src_id) continue;
                auto ch_it = getChannel(*sig_ctx.src_id, dest_id);
                if (ch_it == m_channelMap.end()) continue;

                destinations.push_back(
                    {dest_id, dest_entry.second->getSampleRate(),
                     dest_entry.second->getNodeConfig().getNumChannels(),
                     &dest_entry.second->getNodeConfig(), dest_entry.second,
                     &ch_it->second});
            }
            PROF_TIMER_STOP(dest_snap, sig_ctx.t_dest_snapshot);
        }
        ctx.cached_destinations = destinations;
        ctx.cached_dest_version = current_version;
    }

    // Batch-acquire pool buffers from local NUMA pool with cross-node fallback
    if (!destinations.empty()) {
        bool acquired = false;
        auto& local_pool = m_iq_pools[ctx.numa_node];
        try {
            ctx.pre_acquired_buffers =
                local_pool->acquireBatch(destinations.size());
            acquired = true;
        } catch (const std::runtime_error&) {
            for (size_t i = 0; i < m_iq_pools.size(); i++) {
                if (static_cast<int>(i) == ctx.numa_node) continue;
                try {
                    ctx.pre_acquired_buffers =
                        m_iq_pools[i]->acquireBatch(destinations.size());
                    acquired = true;
                    break;
                } catch (const std::runtime_error&) {
                    continue;
                }
            }
        }
        if (!acquired) {
            m_poolDropCount.fetch_add(destinations.size(),
                                      std::memory_order_relaxed);
            return;
        }
    }

    tbb::parallel_for(size_t(0), destinations.size(), [&](size_t i) {
        ProcessDestinationSignal(i, ctx, destinations, sig_ctx, params,
                                 std::move(ctx.pre_acquired_buffers[i]));
    });
}

void Intermediate::ProcessDestinationSignal(
    size_t dest_index, WorkerContext& ctx,
    const std::vector<DestInfo>& destinations, const SignalContext& sig_ctx,
    const ChannelProcessParams& params,
    dArray_uptr pre_acquired_buffer) {
    auto& dest = destinations[dest_index];

    thread_local std::vector<signal_v> tl_dest_sig_arr;
    thread_local std::vector<void*> tl_output_ptrs;
    thread_local uhd::convert::converter::sptr tl_converter_tx;

    if (!tl_converter_tx) {
        uhd::convert::id_type id_tx;
        id_tx.input_format = "fc32";
        id_tx.num_inputs = 1;
        id_tx.output_format = "sc16_item32_le";
        id_tx.num_outputs = 1;
        tl_converter_tx = uhd::convert::get_converter(id_tx)();
        tl_converter_tx->set_scalar(CONV_SCALAR);
    }

#ifdef ENABLE_PROFILING
    profiling::ProfilingEntry entry{};
    entry.timestamp_ns =
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
            .count();
    entry.src_id = *sig_ctx.src_id;
    entry.dest_id = dest.dest_id;
    entry.src_channels = sig_ctx.src_n_ch;
    entry.sample_rate_hz = sig_ctx.sig_srate;
    entry.freq_mhz = HZ_TO_MHZ(m_freq);
    entry.t_conversion_rx = sig_ctx.t_conversion_rx;
    entry.t_lock_acquire = sig_ctx.t_lock_acquire;
    entry.t_dest_snapshot = sig_ctx.t_dest_snapshot;
    profiling::ManualTimer dest_timer;
    dest_timer.start();
#endif

    auto& channel = *dest.channel;

    struct Header hdr = sig_ctx.sig_hdr;
    float dest_srate = dest.sample_rate;
    size_t dest_sz_ch = sig_ctx.i_sz_ch;
    size_t dest_per_buff = sig_ctx.i_ns_ch;
    const size_t dest_n_ch = dest.num_channels;

    // Ensure thread-local dest buffers are large enough
    if (dest_n_ch > tl_dest_sig_arr.size())
        tl_dest_sig_arr.resize(dest_n_ch,
                               signal_v(MAX_NUM_SAMPLES, fc(0.0f, 0.0f)));

    float sample_ratio = 1.0f;
    if (dest_srate != sig_ctx.sig_srate) {
        sample_ratio = dest_srate / sig_ctx.sig_srate;
        dest_per_buff = std::round(sig_ctx.i_ns_ch * sample_ratio);
        dest_sz_ch = std::round(sig_ctx.i_sz_ch * sample_ratio);
        size_t data_sz = dest_sz_ch * dest_n_ch + HEADER_SIZE;
        hdr.number_of_channels = static_cast<uint8_t>(sig_ctx.src_n_ch);
        hdr.number_of_samples = static_cast<uint32_t>(dest_per_buff);
        hdr.size = data_sz;
        hdr.sample_rate = static_cast<uint64_t>(dest_srate);
    }

    if (sample_ratio == 0.0f) [[unlikely]] {
        if (pre_acquired_buffer)
            m_iq_pools[ctx.numa_node]->release(std::move(pre_acquired_buffer));
        return;
    }

    if (std::isfinite(dest_srate) && dest_srate > 0.0f) {
        const float distance_m = channel.getLinkDistance();
        if (std::isfinite(distance_m) && distance_m > 0.0f) {
            const double delay_s =
                static_cast<double>(distance_m) / SPEED_OF_LIGHT;
            const double sample_period_s =
                1.0 / static_cast<double>(dest_srate);
            if (delay_s >= sample_period_s) {
                dsp::channel::apply_propagation_delay(hdr, delay_s);
            }
        }
    }

#ifdef ENABLE_PROFILING
    entry.num_samples = dest_per_buff;
    entry.dest_channels = dest_n_ch;
#endif

    PROF_TIMER_START(pool_acq);
    dArray_uptr temp_buff = pre_acquired_buffer
                                ? std::move(pre_acquired_buffer)
                                : m_iq_pools[0]->acquire();
    PROF_TIMER_STOP(pool_acq, entry.t_pool_acquire);

    tl_output_ptrs.clear();
    for (size_t ch = 0; ch < dest_n_ch; ch++) {
        tl_output_ptrs.emplace_back(
            reinterpret_cast<void*>(temp_buff.get() + ch * dest_sz_ch));
    }

    ChannelProcessParams local_params = params;
    local_params.s_perBuff = dest_per_buff;
    local_params.sample_ratio = sample_ratio;
    local_params.sample_rate_hz = dest_srate;
    local_params.dest_config = dest.config;

    ChannelProcessTiming local_timing;
    const bool timing_enabled = m_timingEnabled.load(std::memory_order_acquire);
#ifdef ENABLE_PROFILING
    local_params.timing = &local_timing;
    int64_t acc_resample = 0, acc_cir = 0, acc_pathloss = 0;
    int64_t acc_noise = 0, acc_freq_offset = 0;
#else
    if (timing_enabled) {
        local_params.timing = &local_timing;
    }
#endif

    std::vector<ChannelProcessTiming> pending_timings;
    if (timing_enabled) pending_timings.reserve(sig_ctx.src_n_ch);

    PROF_TIMER_START(ch_proc);
#ifdef ENABLE_PROFILING
    int64_t t_conv_tx_total = 0;
#endif

    for (size_t c_dest = 0; c_dest < dest_n_ch; c_dest++) {
        local_params.dest_channel_index = c_dest;
        for (size_t c_src = 0; c_src < sig_ctx.src_n_ch; c_src++) {
            float coeff = channel.getChCoeffs(c_dest, c_src);
            if (coeff == 0.0f) continue;

            local_params.coeff = coeff;
            local_params.src_channel_index = c_src;
            channel.processChannel(ctx.src_sig_arr[c_src], tl_dest_sig_arr[c_dest],
                                   local_params);

#ifdef ENABLE_PROFILING
            if (local_timing.t_resample > local_timing.t_start) {
                acc_resample += local_timing.t_resample - local_timing.t_start;
            }
            if (local_timing.t_cir > local_timing.t_resample) {
                acc_cir += local_timing.t_cir - local_timing.t_resample;
            }
            if (local_timing.t_pathloss > local_timing.t_cir) {
                acc_pathloss += local_timing.t_pathloss - local_timing.t_cir;
            }
            if (local_timing.t_noise > local_timing.t_pathloss) {
                acc_noise += local_timing.t_noise - local_timing.t_pathloss;
            }
            if (local_timing.t_freq_offset > local_timing.t_noise) {
                acc_freq_offset +=
                    local_timing.t_freq_offset - local_timing.t_noise;
            }
#endif
            if (timing_enabled) {
                pending_timings.push_back(local_timing);
            }
        }

        if (timing_enabled && !pending_timings.empty()) {
            ChannelTimingCallback cb_copy;
            {
                std::lock_guard<std::mutex> lock(m_timingMutex);
                cb_copy = m_timingCallback;
            }
            if (cb_copy) {
                for (auto& t : pending_timings) cb_copy(t);
            }
            pending_timings.clear();
        }
#ifdef ENABLE_PROFILING
        profiling::ManualTimer conv_tx_timer;
        conv_tx_timer.start();
#endif
        tl_converter_tx->conv(&tl_dest_sig_arr[c_dest][0], tl_output_ptrs[c_dest],
                              dest_per_buff);
#ifdef ENABLE_PROFILING
        t_conv_tx_total += conv_tx_timer.stop();
#endif
    }

#ifdef ENABLE_PROFILING
    entry.t_channel_process = _prof_timer_ch_proc.stop() - t_conv_tx_total;
    entry.t_conversion_tx = t_conv_tx_total;
#endif

    PROF_TIMER_START(sig_create);
    auto dest_sig = std::make_unique<Signal>(std::move(temp_buff), hdr, m_freq,
                                             dest.dest_id);
    PROF_TIMER_STOP(sig_create, entry.t_signal_create);

    PROF_TIMER_START(queue_add);
#if defined(ENABLE_DEBUG_LOG) || defined(ENABLE_BENCHMARK)
    auto t_end = steady_clock::now();
    int64_t t_end_cnt =
        duration_cast<nanoseconds>(t_end.time_since_epoch()).count();
    dest_sig->setTRecv(sig_ctx.sig_t_recv);
    dest_sig->setTProcStart(sig_ctx.sig_t_proc_start);
    dest_sig->setTProcEnd(t_end_cnt);
#endif
    dest.transmitter->Add2Buff(std::move(dest_sig));
    PROF_TIMER_STOP(queue_add, entry.t_queue_add);

#ifdef ENABLE_PROFILING
    entry.t_total = dest_timer.stop();
    entry.t_resample = acc_resample;
    entry.t_cir = acc_cir;
    entry.t_pathloss = acc_pathloss;
    entry.t_noise = acc_noise;
    entry.t_freq_offset = acc_freq_offset;
    PROF_RECORD(entry);
#endif
}

// ----------------------------------------------------------------------------
// Source & Destination Management
// ----------------------------------------------------------------------------

void Intermediate::AttachDestination(transmitterSPtr transmitter) {
    auto id = transmitter->getNodeConfig().getId();

    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    if (m_destinationMap.find(id) == std::end(m_destinationMap)) {
        m_destinationMap.insert({id, transmitter});
        m_destCount.fetch_add(1, std::memory_order_relaxed);
    } else
        return;

    LOG_INFO(
        "INTERMEDIATE",
        fmt::format(
            "CHEM-Transmitter for NODE \"{}\" is attached to {} MHz Channel!",
            transmitter->getNodeConfig().getName(), HZ_TO_MHZ(m_freq)));

    updateChannelMap();
    m_destVersion.fetch_add(1, std::memory_order_release);
    printStatus();
}

bool Intermediate::DetachDestination(transmitterSPtr transmitter) {
    auto id = transmitter->getId();

    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = m_destinationMap.find(id);

    if (it == std::end(m_destinationMap)) return false;

    transmitter->setAttachState(false);
    removeChannel(transmitter->getId());

    m_destinationMap.erase(it);
    m_destCount.fetch_sub(1, std::memory_order_relaxed);
    m_destVersion.fetch_add(1, std::memory_order_release);

    LOG_INFO(
        "INTERMEDIATE",
        fmt::format(
            "CHEM-Transmitter for NODE \"{}\" is detached from {} MHz Channel!",
            transmitter->getName(), HZ_TO_MHZ(m_freq)));

    EmptyChannelChecker();
    return true;
}

void Intermediate::AttachSource(receiverSPtr receiver) {
    auto id = receiver->getId();

    std::lock_guard<std::mutex> guard(src_mutex);
    auto rx_exist = m_sourceMap.find(id) != std::end(m_sourceMap);

    if (!rx_exist) {
        m_sourceMap.insert(std::make_pair(id, receiver));
        receiver->setAttachState(true);

        auto ctx = std::make_unique<WorkerContext>();
        ctx->receiver_id = id;
        ctx->src_node_id = receiver->getNodeConfig().getId();
        ctx->src_config = &receiver->getNodeConfig();
        InitWorkerConverters(*ctx);

        // Use shared_ptr flag so the lambda remains safe after WorkerContext
        // destruction
        auto running_flag = std::make_shared<std::atomic<bool>>(true);
        ctx->running.store(true, std::memory_order_release);

        WorkerContext* ctx_ptr = ctx.get();

        receiver->setAddQueFnc(
            [ctx_ptr, running_flag, pool = m_iq_pools[0]](
                std::unique_ptr<chem::Signal> sig) {
                if (!sig || !running_flag->load(std::memory_order_acquire)) {
                    if (sig) pool->release(sig->dataArray());
                    return;
                }
                if (ctx_ptr->queue.size() >= QUEUE_CAP - 1) {
                    std::unique_ptr<chem::Signal> _temp;
                    if (ctx_ptr->queue.try_pop(_temp)) {
                        pool->release(_temp->dataArray());
                    }
                }
                if (!ctx_ptr->queue.try_push(std::move(sig))) {
                    if (sig) pool->release(sig->dataArray());
                }
            });

        ctx->worker_thread =
            std::thread(&Intermediate::WorkerLoop, this, std::ref(*ctx));

        {
            std::lock_guard<std::mutex> wguard(m_workers_mutex);
            ctx->running_flag = running_flag;
            m_workers[id] = std::move(ctx);
        }
    } else
        return;

    LOG_INFO(
        "INTERMEDIATE",
        fmt::format(
            "CHEM-Receiver for NODE \"{}\" attached to {} MHz Channel with "
            "dedicated worker!",
            receiver->getName(), HZ_TO_MHZ(m_freq)));
    {
        std::unique_lock<std::shared_mutex> dguard(dest_mutex);
        updateChannelMap();
        m_destVersion.fetch_add(1, std::memory_order_release);
    }
    printStatus();
}

bool Intermediate::DetachSource(receiverSPtr receiver) {
    std::lock_guard<std::mutex> guard(src_mutex);

    auto it = m_sourceMap.find(receiver->getId());

    if (it == std::end(m_sourceMap)) return false;

    auto recv_id = receiver->getId();

    {
        std::lock_guard<std::mutex> wguard(m_workers_mutex);
        auto wit = m_workers.find(recv_id);
        if (wit != m_workers.end() && wit->second->running_flag) {
            wit->second->running_flag->store(false, std::memory_order_release);
        }
    }

    receiver->setAttachState(false);
    // Replace callback with a safe no-op that returns signals to pool
    receiver->setAddQueFnc(
        [pool = m_iq_pools[0]](std::unique_ptr<chem::Signal> sig) {
            if (sig) pool->release(sig->dataArray());
        });

    std::thread worker_thread_to_join;
    {
        std::lock_guard<std::mutex> wguard(m_workers_mutex);
        auto wit = m_workers.find(recv_id);
        if (wit != m_workers.end()) {
            wit->second->queue.abort();
            wit->second->running.store(false, std::memory_order_release);
            worker_thread_to_join = std::move(wit->second->worker_thread);
            // Drain any remaining signals
            std::unique_ptr<chem::Signal> temp;
            while (wit->second->queue.try_pop(temp)) {
                m_iq_pools[0]->release(temp->dataArray());
            }
        }
    }

    if (worker_thread_to_join.joinable()) {
        worker_thread_to_join.join();
    }

    // Now safe to erase worker context (thread has exited)
    {
        std::lock_guard<std::mutex> wguard(m_workers_mutex);
        m_workers.erase(recv_id);
    }

    {
        std::unique_lock<std::shared_mutex> dguard(dest_mutex);
        removeChannel(recv_id);
        m_destVersion.fetch_add(1, std::memory_order_release);
    }
    m_sourceMap.erase(it);

    LOG_INFO("INTERMEDIATE",
             fmt::format("CHEM-Receiver for NODE \"{}\" detached from {} MHz "
                         "Channel, worker stopped!",
                         receiver->getName(), HZ_TO_MHZ(m_freq)));

    EmptyChannelChecker();
    return true;
}

void Intermediate::EmptyChannelChecker() {
    if (isEmpty()) {
        work.store(false, std::memory_order_release);
        cv.notify_all();
        LOG_INFO("INTERMEDIATE",
                 fmt::format("Channel {} MHz is empty. Stopping the channel!",
                             HZ_TO_MHZ(m_freq)));
    }
}

// ----------------------------------------------------------------------------
// Channel Map Management
// ----------------------------------------------------------------------------

void Intermediate::updateChannelMap() {
    for (auto& src : m_sourceMap) {
        for (auto& dest : m_destinationMap) {
            auto dest_id = dest.second->getId();
            auto src_id = src.second->getId();
            auto it = m_channelMap.find({src_id, dest_id});
            if (it == m_channelMap.end()) {
                Channel t_ch(src_id, dest_id, dest.second->getNumChannels(),
                             src.second->getNumChannels());

                // Ensure new channels inherit current pathloss + model
                // configuration.
                auto pathLossType = PathLossType::freeSpace;
                switch (m_propagationModel) {
                    case PropagationModel::TWO_RAY:
                        pathLossType = PathLossType::twoRay;
                        break;
                    case PropagationModel::THREE_GPP_38_901:
                        pathLossType = PathLossType::threeGPP_38_901;
                        break;
                    case PropagationModel::OKUMURA_HATA:
                        pathLossType = PathLossType::okumuraHata;
                        break;
                    case PropagationModel::LONGLEY_RICE:
                        pathLossType = PathLossType::longleyRice;
                        break;
                    case PropagationModel::NONE:
                    case PropagationModel::UNKNOWN:
                    case PropagationModel::FREE_SPACE:
                    default:
                        pathLossType = PathLossType::freeSpace;
                        break;
                }
                t_ch.updatePlType(pathLossType);
                t_ch.update3gppScenario(m_3gppScenario);
                t_ch.updateHataEnvironment(m_hataEnvironment);
                t_ch.updateITMParams(m_itmRefractivity, m_itmGroundConductivity,
                                     m_itmGroundPermittivity, m_itmClimateZone);

                m_channelMap.emplace(std::make_pair(src_id, dest_id), t_ch);
            }
        }
    }
}

void Intermediate::removeChannel(const std::string& nodeId) {
    for (auto it = m_channelMap.begin(); it != m_channelMap.end();) {
        if (it->second.getSrc() == nodeId || it->second.getDest() == nodeId) {
            it = m_channelMap.erase(it);
        } else {
            ++it;
        }
    }
}

std::map<std::pair<std::string, std::string>, chem::Channel>::iterator
Intermediate::getChannel(const std::string& source, const std::string& dest) {
    return m_channelMap.find({source, dest});
}

std::map<std::pair<std::string, std::string>, chem::Channel>&
Intermediate::getChannelList() {
    return m_channelMap;
}

// ----------------------------------------------------------------------------
// Configuration & Parameter Updates
// ----------------------------------------------------------------------------

void Intermediate::setPathLossMode(const std::string& plMode) {
    setPathLossMode(PropagationModelFromString(plMode));
}

void Intermediate::setPathLossMode(PropagationModel plMode) {
    m_propagationModel = plMode;

    auto pathLossType = PathLossType::freeSpace;
    switch (plMode) {
        case PropagationModel::TWO_RAY:
            pathLossType = PathLossType::twoRay;
            break;
        case PropagationModel::THREE_GPP_38_901:
            pathLossType = PathLossType::threeGPP_38_901;
            break;
        case PropagationModel::OKUMURA_HATA:
            pathLossType = PathLossType::okumuraHata;
            break;
        case PropagationModel::LONGLEY_RICE:
            pathLossType = PathLossType::longleyRice;
            break;
        case PropagationModel::NONE:
        case PropagationModel::UNKNOWN:
        case PropagationModel::FREE_SPACE:
        default:
            pathLossType = PathLossType::freeSpace;
            break;
    }

    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    for (auto& entry : m_channelMap) {
        entry.second.updatePlType(pathLossType);
    }
}

void Intermediate::set3gppScenario(const std::string& scenario) {
    if (scenario.empty()) return;
    m_3gppScenario = scenario;
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    for (auto& entry : m_channelMap) {
        entry.second.update3gppScenario(m_3gppScenario);
    }
}

void Intermediate::setHataEnvironment(const std::string& environment) {
    if (environment.empty()) return;
    m_hataEnvironment = environment;
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    for (auto& entry : m_channelMap) {
        entry.second.updateHataEnvironment(m_hataEnvironment);
    }
}

void Intermediate::setITMParams(float refractivity, float ground_conductivity,
                                float ground_permittivity, int climate_zone) {
    m_itmRefractivity = refractivity;
    m_itmGroundConductivity = ground_conductivity;
    m_itmGroundPermittivity = ground_permittivity;
    m_itmClimateZone = std::clamp(climate_zone, 1, 7);
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    for (auto& entry : m_channelMap) {
        entry.second.updateITMParams(m_itmRefractivity, m_itmGroundConductivity,
                                     m_itmGroundPermittivity, m_itmClimateZone);
    }
}

void Intermediate::updatePathLoss(const std::string& plMode,
                                  const float& gr_coeff) {
    updatePathLoss(PropagationModelFromString(plMode), gr_coeff);
}

void Intermediate::updatePathLoss(PropagationModel plMode,
                                  const float& gr_coeff) {
    setPathLossMode(plMode);
    grReflection = gr_coeff;
}

bool Intermediate::updateAWGN(const std::string& src, const std::string& dest,
                              const bool& awgnEnabled, const float& a_snr) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);

    if (it != m_channelMap.end()) {
        if (awgnEnabled) {
            it->second.updateNoiseType(chem::NoiseType::AWGN);
            it->second.updateSNR(a_snr);
        } else {
            it->second.updateNoiseType(chem::NoiseType::NONE);
            it->second.clearSNROverride();
        }
        return true;
    }

    return false;
}

bool Intermediate::updateCIR(const std::string& src, const std::string& dest,
                             const struct chId& ch,
                             const signal_v& taps) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);
    if (it != m_channelMap.end()) {
        if (taps.empty()) {
            it->second.clearChTaps(ch);
        } else {
            it->second.updateChTaps(ch, taps);
        }
        return true;
    }
    return false;
}

void Intermediate::setShadowingSTD(const double& shadowing_std) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    for (auto& entry : m_channelMap) {
        entry.second.setShadowingSTD(shadowing_std);
    }
}

// ----------------------------------------------------------------------------
// PyCHEM & Node Parameter Updates
// ----------------------------------------------------------------------------

bool Intermediate::updateDistance(const std::string& src,
                                  const std::string& dest,
                                  const float& distance) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);

    if (it != m_channelMap.end()) {
        it->second.updateDistance(distance);
        return true;
    }

    return false;
}

bool Intermediate::updateAltitude(const std::string& src,
                                  const std::string& dest,
                                  const float& altitudeDiff) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);

    if (it != m_channelMap.end()) {
        it->second.updateAltitude(altitudeDiff);
        return true;
    }

    return false;
}

bool Intermediate::updateElevation(const std::string& src,
                                   const std::string& dest,
                                   const float& elevation) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);

    if (it != m_channelMap.end()) {
        it->second.updateElevation(elevation);
        return true;
    }

    return false;
}

bool Intermediate::updateAzimuth(const std::string& src,
                                 const std::string& dest,
                                 const float& azimuth) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);

    if (it != m_channelMap.end()) {
        it->second.updateAzimuth(azimuth);
        return true;
    }

    return false;
}

bool Intermediate::updateChannelCoeff(const std::string& src,
                                      const std::string& dest,
                                      const struct chId& ch,
                                      const float& coeff) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);

    if (it != m_channelMap.end()) {
        it->second.updateChCoeff(ch, coeff);
        return true;
    }

    return false;
}

bool Intermediate::updateFrequencyOffset(const std::string& src,
                                         const std::string& dest,
                                         const double& freq_offset_hz) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);
    if (it != m_channelMap.end()) {
        it->second.setFrequencyOffsetHz(freq_offset_hz);
        return true;
    }
    return false;
}

bool Intermediate::updateDopplerEnabled(const std::string& src,
                                        const std::string& dest,
                                        const bool& enabled) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);
    if (it != m_channelMap.end()) {
        it->second.setDopplerEnabled(enabled);
        return true;
    }
    return false;
}

bool Intermediate::updateDopplerHz(const std::string& src,
                                   const std::string& dest,
                                   const double& doppler_hz) {
    std::unique_lock<std::shared_mutex> guard(dest_mutex);
    auto it = getChannel(src, dest);
    if (it != m_channelMap.end()) {
        it->second.setDopplerHz(doppler_hz);
        return true;
    }
    return false;
}

bool Intermediate::updateNodeAntenna(const std::string& nodeName,
                                     const std::string& pattern,
                                     const std::string& txPattern,
                                     const std::string& rxPattern) {
    bool updated = false;

    {
        std::lock_guard<std::mutex> guard(src_mutex);
        for (auto& src_entry : m_sourceMap) {
            if (src_entry.second->getName() == nodeName ||
                src_entry.second->getId() == nodeName) {
                auto& cfg = src_entry.second->getNodeConfig();
                if (!pattern.empty()) {
                    cfg.setAntennaPattern(pattern);
                } else {
                    if (!txPattern.empty()) cfg.setTxAntennaPattern(txPattern);
                    if (!rxPattern.empty()) cfg.setRxAntennaPattern(rxPattern);
                }
                updated = true;
            }
        }
    }

    {
        std::unique_lock<std::shared_mutex> guard(dest_mutex);
        for (auto& dest_entry : m_destinationMap) {
            if (dest_entry.second->getName() == nodeName ||
                dest_entry.second->getId() == nodeName) {
                auto& cfg = dest_entry.second->getNodeConfig();
                if (!pattern.empty()) {
                    cfg.setAntennaPattern(pattern);
                } else {
                    if (!txPattern.empty()) cfg.setTxAntennaPattern(txPattern);
                    if (!rxPattern.empty()) cfg.setRxAntennaPattern(rxPattern);
                }
                updated = true;
            }
        }
    }

    return updated;
}

// ----------------------------------------------------------------------------
// Status & Monitoring
// ----------------------------------------------------------------------------

void Intermediate::printStatus() {
    LOG_INFO(
        "INTERMEDIATE",
        fmt::format("Channel {} MHz Status: TXs: {} | RXs: {} | Channels: {}",
                    HZ_TO_MHZ(m_freq), GetDestinationCnt(), GetSourceCnt(),
                    m_channelMap.size()));
    for (auto& ch_entry : m_channelMap) {
        LOG_INFO("INTERMEDIATE",
                 fmt::format("  Channel: Source \"{}\" -> Destination \"{}\"",
                             m_sourceMap[ch_entry.second.getSrc()]
                                 ->getNodeConfig()
                                 .getName(),
                             m_destinationMap[ch_entry.second.getDest()]
                                 ->getNodeConfig()
                                 .getName()));
    }
}

bool Intermediate::isEmpty() const {
    return m_destinationMap.empty() && m_sourceMap.empty();
}

uint8_t Intermediate::GetDestinationCnt() const {
    return static_cast<uint8_t>(m_destinationMap.size());
}

uint8_t Intermediate::GetSourceCnt() const {
    return static_cast<uint8_t>(m_sourceMap.size());
}

const double Intermediate::GetFreq() const { return m_freq; }

PropagationModel Intermediate::getPathLossMode() const {
    return m_propagationModel;
}

void Intermediate::setMaxLatency(int64_t max_latency_ms) {
    m_max_latency_ns = MS_2_NS(static_cast<float>(max_latency_ms));
}

int64_t Intermediate::getMaxLatency() const {
    return static_cast<int64_t>(NS_2_MS(static_cast<float>(m_max_latency_ns)));
}

// ----------------------------------------------------------------------------
// Timing & Profiling
// ----------------------------------------------------------------------------

void Intermediate::setTimingCallback(ChannelTimingCallback callback) {
    {
        std::lock_guard<std::mutex> lock(m_timingMutex);
        m_timingCallback = std::move(callback);
    }
    m_timingEnabled.store(true, std::memory_order_release);
}

void Intermediate::clearTimingCallback() {
    m_timingEnabled.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_timingMutex);
        m_timingCallback = nullptr;
    }
}

bool Intermediate::isTimingEnabled() const {
    return m_timingEnabled.load(std::memory_order_acquire);
}

void Intermediate::setSignalProcessingCallback(
    SignalProcessingCallback callback) {
    m_signalProcessingCallback = std::move(callback);
    m_signalProcessingEnabled.store(true, std::memory_order_release);
}

void Intermediate::clearSignalProcessingCallback() {
    m_signalProcessingEnabled.store(false, std::memory_order_release);
    m_signalProcessingCallback = nullptr;
}

// ----------------------------------------------------------------------------
// Misc / Fallback
// ----------------------------------------------------------------------------

void Intermediate::add2Queue(std::unique_ptr<chem::Signal> sig) {
    // Fallback: release signals that aren't routed through worker queues
    if (sig) {
        m_iq_pools[0]->release(sig->dataArray());
    }
}
