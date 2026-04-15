#include "chem/dsp/utils.h"

#include <cmath>
#include <numeric>
#include <vector>

#include <volk/volk.h>

namespace chem {
namespace dsp {
namespace utils {

// ----------------------------------------------------------------------------
// Math & Filter Helpers
// ----------------------------------------------------------------------------

float bessel_i0(float x) {
    // Ref. GNURadio gr-filter/firdes
    float abs_x, sum;
    float y;

    abs_x = std::fabs(x);

    if (abs_x < 3.75f) {
        y = x / 3.75f;
        y *= y;
        sum = 1.0f + y * (3.5156229f +
                          y * (3.0899424f +
                               y * (1.2067492f +
                                    y * (0.2659732f + y * (0.360768e-1f +
                                                           y * 0.45813e-2f)))));
    } else {
        y = 3.75f / abs_x;
        sum = (std::exp(abs_x) / std::sqrt(abs_x)) *
              (0.39894228f +
               y * (0.1328592e-1f +
                    y * (0.225319e-2f +
                         y * (-0.157565e-2f +
                              y * (0.916281e-2f +
                                   y * (-0.2057706e-1f +
                                        y * (0.2635537e-1f +
                                             y * (-0.1647633e-1f +
                                                  y * 0.392377e-2f))))))));
    }
    return sum;
}

int calcNTaps(float sample_rate, float transition_width, float beta) {
    // B_T = (F_stop - F_pass) / F_s
    // N_taps = Attenuation / (22 * B_T)
    float attenuation = (beta / 0.1102f + 8.7f);
    int ntaps = static_cast<int>(attenuation / (22.0f * transition_width));

    if ((ntaps & 1) == 0)  // if even...
        ntaps++;           // ...make odd

    return ntaps;
}

std::vector<float> kaiserWin(int num_taps, float beta) {
    std::vector<float> window(num_taps);

    float denominator = bessel_i0(beta);

    window[0] = 1.0f / denominator;
    for (int i = 1; i < num_taps; i++) {
        float x = 2.0f * i / (num_taps - 1) - 1.0f;
        window[i] = bessel_i0(beta * std::sqrt(1.0f - x * x)) / denominator;
    }
    window[num_taps - 1] = 1.0f / denominator;

    return window;
}

std::vector<float> lowpassKaiserWin(int num_taps, float cutoff, float beta,
                                    int up_factor) {
    float gain = static_cast<float>(up_factor);
    std::vector<float> taps(num_taps);
    std::vector<float> window = kaiserWin(num_taps, beta);

    int half = (num_taps - 1) / 2;
    cutoff /= static_cast<float>(up_factor);
    double fwT0 = 2.0 * M_PI * static_cast<double>(cutoff);

    for (int i = 0; i < num_taps; i++) {
        if (i == half) {
            taps[i] = 2.0f * cutoff;
        } else {
            float x = static_cast<float>(fwT0 * (i - half));
            taps[i] = std::sin(x) / (static_cast<float>(M_PI) * (i - half));
        }
        taps[i] *= window[i];
    }

    // Normalize to ensure unit gain at DC
    double fmax = static_cast<double>(taps[half]);
    for (int n = 1; n <= half; n++) {
        fmax += 2.0 * static_cast<double>(taps[n + half]);
    }

    for (auto& tap : taps) {
        tap *= gain;
    }

    return taps;
}

void computeResamplingFactors(double ratio, int& up, int& down) {
    const int maxFactor = 1000;
    int num = static_cast<int>(std::round(ratio * maxFactor));
    int denom = maxFactor;

    // Simplify fraction using std::gcd
    int common_factor = std::gcd(num, denom);
    up = num / common_factor;
    down = denom / common_factor;
}

// ----------------------------------------------------------------------------
// SIMD Optimized DSP Kernels (VOLK)
// ----------------------------------------------------------------------------

void fc32_mul(const signal_v& input1, const signal_v& input2,
              signal_v& output, size_t size) {
    volk_32fc_x2_multiply_32fc(
        reinterpret_cast<lv_32fc_t*>(output.data()),
        reinterpret_cast<const lv_32fc_t*>(input1.data()),
        reinterpret_cast<const lv_32fc_t*>(input2.data()),
        static_cast<unsigned int>(size));
}

void fc32_mul_scalar(const signal_v& input, fc scalar, signal_v& output,
                     size_t size) {
    lv_32fc_t s;
    reinterpret_cast<float*>(&s)[0] = scalar.real();
    reinterpret_cast<float*>(&s)[1] = scalar.imag();
    volk_32fc_s32fc_multiply_32fc(
        reinterpret_cast<lv_32fc_t*>(output.data()),
        reinterpret_cast<const lv_32fc_t*>(input.data()),
        s,
        static_cast<unsigned int>(size));
}

void fc32_mul_scalar(const signal_v& input, float scalar, signal_v& output,
                     size_t size) {
    lv_32fc_t s;
    reinterpret_cast<float*>(&s)[0] = scalar;
    reinterpret_cast<float*>(&s)[1] = 0.0f;
    volk_32fc_s32fc_multiply_32fc(
        reinterpret_cast<lv_32fc_t*>(output.data()),
        reinterpret_cast<const lv_32fc_t*>(input.data()),
        s,
        static_cast<unsigned int>(size));
}

void fc32_lshift(const signal_v& input, uint8_t shift, signal_v& output,
                 size_t size) {
    const float scale = std::ldexp(1.0f, static_cast<int>(shift));
    lv_32fc_t s;
    reinterpret_cast<float*>(&s)[0] = scale;
    reinterpret_cast<float*>(&s)[1] = 0.0f;
    volk_32fc_s32fc_multiply_32fc(
        reinterpret_cast<lv_32fc_t*>(output.data()),
        reinterpret_cast<const lv_32fc_t*>(input.data()),
        s,
        static_cast<unsigned int>(size));
}

void fc32_rshift(const signal_v& input, uint8_t shift, signal_v& output,
                 size_t size) {
    const float scale = std::ldexp(1.0f, -static_cast<int>(shift));
    lv_32fc_t s;
    reinterpret_cast<float*>(&s)[0] = scale;
    reinterpret_cast<float*>(&s)[1] = 0.0f;
    volk_32fc_s32fc_multiply_32fc(
        reinterpret_cast<lv_32fc_t*>(output.data()),
        reinterpret_cast<const lv_32fc_t*>(input.data()),
        s,
        static_cast<unsigned int>(size));
}

void fc32_mul_scalar_sum(const signal_v& input1, fc scalar,
                         const signal_v& input2, signal_v& output,
                         size_t size) {
    // output = input1 * scalar + input2
    // Step 1: output = input1 * scalar
    lv_32fc_t s;
    reinterpret_cast<float*>(&s)[0] = scalar.real();
    reinterpret_cast<float*>(&s)[1] = scalar.imag();
    volk_32fc_s32fc_multiply_32fc(
        reinterpret_cast<lv_32fc_t*>(output.data()),
        reinterpret_cast<const lv_32fc_t*>(input1.data()),
        s,
        static_cast<unsigned int>(size));
    // Step 2: output += input2 (treat as 2*size floats for element-wise add)
    volk_32f_x2_add_32f(
        reinterpret_cast<float*>(output.data()),
        reinterpret_cast<const float*>(output.data()),
        reinterpret_cast<const float*>(input2.data()),
        static_cast<unsigned int>(2 * size));
}

void fc32_mul_scalar_sum(const signal_v& input1, float scalar,
                         const signal_v& input2, signal_v& output,
                         size_t size) {
    // output = input1 * scalar + input2
    // Step 1: output = input1 * scalar
    lv_32fc_t s;
    reinterpret_cast<float*>(&s)[0] = scalar;
    reinterpret_cast<float*>(&s)[1] = 0.0f;
    volk_32fc_s32fc_multiply_32fc(
        reinterpret_cast<lv_32fc_t*>(output.data()),
        reinterpret_cast<const lv_32fc_t*>(input1.data()),
        s,
        static_cast<unsigned int>(size));
    // Step 2: output += input2
    volk_32f_x2_add_32f(
        reinterpret_cast<float*>(output.data()),
        reinterpret_cast<const float*>(output.data()),
        reinterpret_cast<const float*>(input2.data()),
        static_cast<unsigned int>(2 * size));
}

void fc32_sum(const signal_v& input1, const signal_v& input2,
              signal_v& output, size_t size) {
    volk_32f_x2_add_32f(
        reinterpret_cast<float*>(output.data()),
        reinterpret_cast<const float*>(input1.data()),
        reinterpret_cast<const float*>(input2.data()),
        static_cast<unsigned int>(2 * size));
}

}  // namespace utils
}  // namespace dsp
}  // namespace chem
