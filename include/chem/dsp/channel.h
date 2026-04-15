#pragma once

#include <complex>
#include <cstdint>
#include <vector>

#include "../common.h"

namespace chem {
namespace dsp {
namespace channel {

enum class CIRApplyMethod : uint8_t {
    NONE = 0,
    FIR_CAUSAL = 1,
    FIR_CENTERED = 2,
    DIRECT_CONV = 3,
};

// Noise
const signal_v& get_noise(const size_t size, double bandwidth = 1.0,
                           NoiseType type = NoiseType::AWGN);
const signal_v& get_noise(const size_t size, double bandwidth, NoiseType type,
                           double noise_figure_db);

// Channel Impulse Response (CIR) / multipath
void apply_cir(const signal_v& input, signal_v& output, const signal_v& taps,
               size_t size, CIRApplyMethod method = CIRApplyMethod::FIR_CAUSAL);

// Private helpers for apply_cir (internal use).
void apply_cir_fir_causal(const signal_v& input, signal_v& output,
                          const signal_v& taps, size_t size);
void apply_cir_direct_conv(const signal_v& input, signal_v& output,
                           const signal_v& taps, size_t size);

// Impairments
void apply_propagation_delay(Header& header, double delay_s);
void apply_doppler(signal_v& signal, const size_t size, double sample_rate_hz,
                   double doppler_hz, double& phase_rad);

// Path Loss
float calc_fspl(const float& distance_m, const float& frequency_hz,
                const double& shadowing_std);

float calc_3gpp_38_901(const float& distance_m, const float& frequency_hz,
                       const float& ue_height, const float& bs_height,
                       const std::string& scenario,
                       const double& shadowing_std);

float calc_2ray(const float& distance_m, const float& frequency_hz,
                const float& tx_height, const float& rx_height,
                const float& ground_coeff, const double& shadowing_std);

float calc_okumura_hata(const float& distance_m, const float& frequency_hz,
                        const float& bs_height, const float& ms_height,
                        const std::string& environment,
                        const double& shadowing_std);

float calc_longley_rice(const float& distance_m, const float& frequency_hz,
                        const float& tx_height, const float& rx_height,
                        const float& refractivity,
                        const float& ground_conductivity,
                        const float& ground_permittivity,
                        const int& climate_zone, const double& shadowing_std);

// Apply Additive White Gaussian Noise (AWGN)
void apply_awgn(const signal_v& input, signal_v& output);

}  // namespace channel
}  // namespace dsp
}  // namespace chem
