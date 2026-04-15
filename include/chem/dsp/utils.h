#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "chem/common.h"

namespace chem {
namespace dsp {
namespace utils {

// ----------------------------------------------------------------------------
// Math & Filter Helpers
// ----------------------------------------------------------------------------

float bessel_i0(float x);

int calcNTaps(float sample_rate, float transition_width, float beta);

std::vector<float> kaiserWin(int num_taps, float beta);

std::vector<float> lowpassKaiserWin(int num_taps, float cutoff, float beta,
                                    int up_factor);

void computeResamplingFactors(double ratio, int& up, int& down);

// ----------------------------------------------------------------------------
// SIMD Optimized DSP Kernels (Complex Float 32-bit)
// ----------------------------------------------------------------------------

void fc32_mul(const signal_v& input1, const signal_v& input2,
              signal_v& output, size_t size);

void fc32_mul_scalar(const signal_v& input, fc scalar, signal_v& output,
                     size_t size);

void fc32_mul_scalar(const signal_v& input, float scalar, signal_v& output,
                     size_t size);

void fc32_lshift(const signal_v& input, uint8_t shift, signal_v& output,
                 size_t size);

void fc32_rshift(const signal_v& input, uint8_t shift, signal_v& output,
                 size_t size);

void fc32_mul_scalar_sum(const signal_v& input1, fc scalar,
                         const signal_v& input2, signal_v& output,
                         size_t size);

void fc32_mul_scalar_sum(const signal_v& input1, float scalar,
                         const signal_v& input2, signal_v& output,
                         size_t size);

void fc32_sum(const signal_v& input1, const signal_v& input2,
              signal_v& output, size_t size);

}  // namespace utils
}  // namespace dsp
}  // namespace chem
