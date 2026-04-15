// signal.h
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

#include "../common.h"

namespace chem {
namespace dsp {
namespace signal {

float calc_power(const signal_v& in1, const size_t size);
float calc_peak_power(const signal_v& in1, const size_t size);
std::vector<float> calc_psd(const signal_v& in1, const size_t size);
void resample(const signal_v& src, signal_v& dest, const float& rate,
              const size_t size);

void fir_filter(const signal_v& signal, const signal_v& coeffs,
                signal_v& filtered, const size_t size);
void fir_filter(const signal_v& signal, const std::vector<float> coeffs,
                signal_v& filtered, const size_t size);
void downsample(const signal_v& src, signal_v& dest, const uint16_t down,
                const size_t size);
void upsample(const signal_v& src, signal_v& dest, const uint16_t up,
              const size_t size);

// Mix a complex exponential into the signal to simulate frequency offset (CFO).
// `phase_rad` is an in/out accumulator to preserve phase continuity across
// buffers.
void apply_frequency_offset(signal_v& signal, size_t size,
                            double sample_rate_hz, double freq_offset_hz,
                            double& phase_rad);

}  // namespace signal
}  // namespace dsp
}  // namespace chem
