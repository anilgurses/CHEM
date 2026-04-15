#include "chem/dsp/channel.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>

#include "chem/channel_models/TR_38_901_3GPP.hpp"
#include "chem/channel_models/longley_rice.hpp"
#include "chem/channel_models/okumura_hata.hpp"
#include "chem/common.h"
#include "chem/dsp/signal.h"
#include "chem/dsp/utils.h"

using chem::dsp::channel::CIRApplyMethod;
using chem::signal_v;

// ----------------------------------------------------------------------------
// Internal Helpers
// ----------------------------------------------------------------------------

/**
 * @brief Thread-local RNG for path loss and noise calculations.
 * Avoids expensive per-call RNG creation and ensures thread safety.
 */
static std::mt19937& get_thread_local_rng() {
    static thread_local std::mt19937 gen(std::random_device{}());
    return gen;
}

static double safe_noise_bw(double bandwidth) {
    if (!std::isfinite(bandwidth) || bandwidth <= 0.0) {
        return 1.0;
    }
    return bandwidth;
}

static float noise_floor_dbfs(double bandwidth, double noise_figure_db) {
    const double bw_safe = safe_noise_bw(bandwidth);
    const double nf =
        std::isfinite(noise_figure_db) ? noise_figure_db : NOISE_FIGURE_RX;
    return static_cast<float>(NOISE_POWER_DBFS + 10.0 * std::log10(bw_safe) +
                              nf);
}

// ----------------------------------------------------------------------------
// Path Loss Calculations
// ----------------------------------------------------------------------------

float chem::dsp::channel::calc_fspl(const float& distance_m,
                                    const float& frequency_hz,
                                    const double& shadowing_std) {
    double pl_db = 0.0;

    try {
        // Calculate Free Space Path Loss (dB)
        // FSPL = 20log10(d) + 20log10(f) + 20log10(4pi/c)
        pl_db = 20.0 * std::log10(frequency_hz) +
                20.0 * std::log10(distance_m + LOG_SAFE_GUARD) + FSPL_CONSTANT;
    } catch (const std::exception& e) {
        return 0.0f;
    }

    // Apply log-normal shadowing (in dB) directly on path loss
    if (shadowing_std > 0.0) {
        std::normal_distribution<float> shadowingDist(SHADOWING_MEAN,
                                                      shadowing_std);
        pl_db += shadowingDist(get_thread_local_rng());
    }

    return static_cast<float>(pl_db);
}

float chem::dsp::channel::calc_3gpp_38_901(const float& distance_m,
                                           const float& frequency_hz,
                                           const float& ue_height,
                                           const float& bs_height,
                                           const std::string& scenario,
                                           const double& shadowing_std) {
    double pl_db = 0.0;
    try {
        if (std::isfinite(frequency_hz) && frequency_hz > 0.0f) {
            pl_db = chem::channel_models::tr_38_901::calculate_path_loss(
                scenario, distance_m, frequency_hz / 1e9f, ue_height,
                bs_height);
        }
    } catch (...) {
        pl_db = 0.0;
    }

    if (shadowing_std > 0.0) {
        std::normal_distribution<float> shadowingDist(SHADOWING_MEAN,
                                                      shadowing_std);
        pl_db += static_cast<double>(shadowingDist(get_thread_local_rng()));
    }

    return static_cast<float>(pl_db);
}

float chem::dsp::channel::calc_2ray(const float& distance_m,
                                    const float& frequency_hz,
                                    const float& tx_height,
                                    const float& rx_height,
                                    const float& ground_coeff,
                                    const double& shadowing_std) {
    const float d = std::max<float>(0.0f, distance_m);
    const float ht = std::max<float>(0.01f, tx_height);
    const float hr = std::max<float>(0.01f, rx_height);

    const double d1 = std::sqrt(d * d + std::pow(ht - hr, 2.0f));
    const double d2 = std::sqrt(d * d + std::pow(ht + hr, 2.0f));
    const double gamma = std::isfinite(ground_coeff)
                             ? std::clamp<double>(ground_coeff, -1.0, 1.0)
                             : -1.0;

    double pl_db = 0.0;
    try {
        if (std::isfinite(frequency_hz) && frequency_hz > 0.0f) {
            // Two-ray ground reflection model:
            // Pr/Pt = (lambda/4pi)^2 * |e^{-jkd1}/d1 + Gamma * e^{-jkd2}/d2|^2
            const double lambda = static_cast<double>(SPEED_OF_LIGHT) /
                                  static_cast<double>(frequency_hz);
            const double k = 2.0 * PI / std::max(1e-12, lambda);
            const double phase = k * (d2 - d1);

            const double a = 1.0 / std::max(1e-12, d1);
            const double b = gamma / std::max(1e-12, d2);
            const double e_mag2 =
                std::max(1e-20, a * a + b * b + 2.0 * a * b * std::cos(phase));

            const double pl_lin = std::pow((4.0 * PI) / lambda, 2.0) / e_mag2;
            pl_db = 10.0 * std::log10(std::max(1e-20, pl_lin));
        }
    } catch (...) {
        pl_db = 0.0;
    }

    if (shadowing_std > 0.0) {
        std::normal_distribution<float> shadowingDist(SHADOWING_MEAN,
                                                      shadowing_std);
        pl_db += static_cast<double>(shadowingDist(get_thread_local_rng()));
    }

    return static_cast<float>(pl_db);
}

float chem::dsp::channel::calc_okumura_hata(const float& distance_m,
                                            const float& frequency_hz,
                                            const float& bs_height,
                                            const float& ms_height,
                                            const std::string& environment,
                                            const double& shadowing_std) {
    double pl_db = 0.0;
    try {
        if (std::isfinite(frequency_hz) && frequency_hz > 0.0f) {
            auto env =
                chem::channel_models::okumura_hata::EnvironmentFromString(
                    environment);
            pl_db = chem::channel_models::okumura_hata::calculate_path_loss(
                static_cast<double>(frequency_hz),
                static_cast<double>(distance_m), static_cast<double>(bs_height),
                static_cast<double>(ms_height), env);
        }
    } catch (...) {
        pl_db = 0.0;
    }

    if (shadowing_std > 0.0) {
        std::normal_distribution<float> shadowingDist(SHADOWING_MEAN,
                                                      shadowing_std);
        pl_db += static_cast<double>(shadowingDist(get_thread_local_rng()));
    }

    return static_cast<float>(pl_db);
}

float chem::dsp::channel::calc_longley_rice(
    const float& distance_m, const float& frequency_hz, const float& tx_height,
    const float& rx_height, const float& refractivity,
    const float& ground_conductivity, const float& ground_permittivity,
    const int& climate_zone, const double& shadowing_std) {
    double pl_db = 0.0;
    try {
        if (std::isfinite(frequency_hz) && frequency_hz > 0.0f) {
            chem::channel_models::longley_rice::ITMParams params;
            params.freq_hz = static_cast<double>(frequency_hz);
            params.tx_height_m =
                static_cast<double>(std::max(0.01f, tx_height));
            params.rx_height_m =
                static_cast<double>(std::max(0.01f, rx_height));
            params.refractivity = static_cast<double>(refractivity);
            params.ground_conductivity =
                static_cast<double>(ground_conductivity);
            params.ground_permittivity =
                static_cast<double>(ground_permittivity);
            params.climate =
                static_cast<chem::channel_models::longley_rice::Climate>(
                    std::clamp(climate_zone, 1, 7));

            pl_db = chem::channel_models::longley_rice::calculate_path_loss(
                static_cast<double>(distance_m), params);
        }
    } catch (...) {
        pl_db = 0.0;
    }

    if (shadowing_std > 0.0) {
        std::normal_distribution<float> shadowingDist(SHADOWING_MEAN,
                                                      shadowing_std);
        pl_db += static_cast<double>(shadowingDist(get_thread_local_rng()));
    }

    return static_cast<float>(pl_db);
}

// ----------------------------------------------------------------------------
// Noise Generation and Application
// ----------------------------------------------------------------------------

const signal_v& chem::dsp::channel::get_noise(const size_t size,
                                               double bandwidth,
                                               NoiseType type) {
    return get_noise(size, bandwidth, type, NOISE_FIGURE_RX);
}

const signal_v& chem::dsp::channel::get_noise(const size_t size,
                                               double bandwidth, NoiseType type,
                                               double noise_figure_db) {
    // Thread-local storage for lock-free parallel access
    thread_local std::unique_ptr<signal_v> _noise;
    thread_local std::mt19937 gen(std::random_device{}());
    thread_local double cached_bw = 0.0;
    thread_local double cached_nf = 0.0;
    thread_local float cached_sigma = 0.0f;
    thread_local bool needs_regen = true;

    const double nf =
        std::isfinite(noise_figure_db) ? noise_figure_db : NOISE_FIGURE_RX;

    // Regenerate when bandwidth or noise figure changes
    if (std::abs(bandwidth - cached_bw) > 1e-6 ||
        std::abs(nf - cached_nf) > 1e-6) {
        cached_bw = bandwidth;
        cached_nf = nf;
        const float power_dbfs = noise_floor_dbfs(bandwidth, nf);
        const float power_lin = static_cast<float>(DB_TO_LIN(power_dbfs));
        cached_sigma = std::sqrt(std::max(0.0f, power_lin) / 2.0f);
        needs_regen = true;
    }

    // Keep shuffles in L1/L2 cache
    if (!_noise || _noise->size() != size) {
        _noise.reset(new signal_v(size, fc(0.0f, 0.0f)));
        needs_regen = true;
    }

    if (type == NoiseType::NONE) {
        std::fill_n(_noise->begin(), size, fc(0.0f, 0.0f));
        return *_noise;
    }

    if (needs_regen) {
        // Full generation: only on first call or parameter change
        std::normal_distribution<float> n_distribution(0.0f, cached_sigma);
        for (size_t i = 0; i < size; ++i) {
            (*_noise)[i] = fc(n_distribution(gen), n_distribution(gen));
        }
        needs_regen = false;
    } else {
        // Shuffle existing noise — O(n) with cache-friendly access
        std::shuffle(_noise->begin(), _noise->begin() + size, gen);
    }

    return *_noise;
}

// This is depreccated and not being called anywhere
// NOTE: I might remove it
void chem::dsp::channel::apply_awgn(const signal_v& input, signal_v& output) {
    const size_t size = input.size();
    if (size == 0) return;
    if (output.size() < size) output.resize(size);

    const signal_v& noise = get_noise(size);
    utils::fc32_sum(input, noise, output, size);
}

// ----------------------------------------------------------------------------
// CIR
// ----------------------------------------------------------------------------

void chem::dsp::channel::apply_cir_fir_causal(const signal_v& input,
                                              signal_v& output,
                                              const signal_v& taps,
                                              size_t size) {
    if (size == 0) return;
    if (taps.empty()) {
        if (&input != &output) {
            std::copy_n(input.begin(), size, output.begin());
        }
        return;
    }

    const size_t in_size = std::min(size, input.size());
    if (output.size() < in_size) output.resize(in_size);

    const size_t tap_len = taps.size();
    if (tap_len == 1) {
        // Single-tap fast path: output = taps[0] * input
        lv_32fc_t s;
        reinterpret_cast<float*>(&s)[0] = taps[0].real();
        reinterpret_cast<float*>(&s)[1] = taps[0].imag();
        volk_32fc_s32fc_multiply_32fc(
            reinterpret_cast<lv_32fc_t*>(output.data()),
            reinterpret_cast<const lv_32fc_t*>(input.data()),
            s,
            static_cast<unsigned int>(in_size));
        return;
    }

    // Build reversed taps for causal convolution
    signal_v rev_taps(tap_len);
    for (size_t j = 0; j < tap_len; ++j) {
        rev_taps[j] = taps[tap_len - 1 - j];
    }

    const size_t pad = tap_len - 1;
    signal_v extended(in_size + pad, fc(0.0f, 0.0f));
    std::copy_n(input.begin(), in_size, extended.begin() + pad);

    for (size_t i = 0; i < in_size; ++i) {
        lv_32fc_t dot;
        volk_32fc_x2_dot_prod_32fc(
            &dot,
            reinterpret_cast<const lv_32fc_t*>(&extended[i]),
            reinterpret_cast<const lv_32fc_t*>(rev_taps.data()),
            static_cast<unsigned int>(tap_len));
        output[i] = fc(reinterpret_cast<float*>(&dot)[0],
                        reinterpret_cast<float*>(&dot)[1]);
    }
}

void chem::dsp::channel::apply_cir_direct_conv(const signal_v& input,
                                               signal_v& output,
                                               const signal_v& taps,
                                               size_t size) {
    if (size == 0) return;
    const size_t in_size = std::min(size, input.size());
    if (output.size() < in_size) output.resize(in_size);
    if (taps.empty()) {
        if (&input != &output) {
            std::copy_n(input.begin(), in_size, output.begin());
        }
        return;
    }

    const size_t tap_len = taps.size();
    for (size_t n = 0; n < in_size; ++n) {
        fc acc(0.0f, 0.0f);
        const size_t k_max = std::min(n, tap_len - 1);
        for (size_t k = 0; k <= k_max; ++k) {
            acc += taps[k] * input[n - k];
        }
        output[n] = acc;
    }
}

void chem::dsp::channel::apply_cir(const signal_v& input, signal_v& output,
                                   const signal_v& taps, size_t size,
                                   CIRApplyMethod method) {
    if (size == 0) return;

    switch (method) {
        case CIRApplyMethod::NONE:
            if (&input != &output) {
                const size_t in_size = std::min(size, input.size());
                if (output.size() < in_size) output.resize(in_size);
                std::copy_n(input.begin(), in_size, output.begin());
            }
            return;
        case CIRApplyMethod::FIR_CENTERED:
            dsp::signal::fir_filter(input, taps, output,
                                    std::min(size, input.size()));
            return;
        case CIRApplyMethod::DIRECT_CONV:
            apply_cir_direct_conv(input, output, taps, size);
            return;
        case CIRApplyMethod::FIR_CAUSAL:
        default:
            apply_cir_fir_causal(input, output, taps, size);
            return;
    }
}

// ----------------------------------------------------------------------------
// Signal Impairments
// ----------------------------------------------------------------------------

void chem::dsp::channel::apply_propagation_delay(Header& header,
                                                 double delay_s) {
    const int64_t delay_ns = static_cast<int64_t>(delay_s * 1.0e9);
    header.start += delay_ns;
    header.end += delay_ns;
    header.rt_tx_time += delay_ns;
}

void chem::dsp::channel::apply_doppler(signal_v& signal, const size_t size,
                                       double sample_rate_hz, double doppler_hz,
                                       double& phase_rad) {
    dsp::signal::apply_frequency_offset(signal, size, sample_rate_hz,
                                        doppler_hz, phase_rad);
}
