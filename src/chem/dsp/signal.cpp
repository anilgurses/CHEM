/* signal.cpp
 *
 * Copyright (C) 2022 Anil Gurses
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * @author Anil Gurses <agurses@ncsu.edu>
 * @date 11/10/2023
 *
 */

#include "chem/dsp/signal.h"

#include <volk/volk.h>

#include "chem/common.h"
#include "chem/dsp/utils.h"

namespace chem {
namespace dsp {
namespace signal {

void fir_filter(const signal_v& signal, const signal_v& coeffs,
                signal_v& filtered, const size_t size) {
    size_t signal_len = signal.size();
    size_t coeff_len = coeffs.size();
    size_t delay = (coeff_len - 1) / 2;

    int ext_signal_len = signal_len + delay * 2;
    signal_v extended_signal(ext_signal_len);

    for (size_t i = 0; i < delay; i++) {
        extended_signal[i] = signal[0];
    }
    std::copy(signal.begin(), signal.begin() + size,
              extended_signal.begin() + delay);
    for (size_t i = 0; i < delay; i++) {
        extended_signal[delay + signal_len + i] = signal[signal_len - 1];
    }

    for (size_t i = 0; i < signal_len; ++i) {
        lv_32fc_t dot;
        volk_32fc_x2_dot_prod_32fc(
            &dot,
            reinterpret_cast<const lv_32fc_t*>(&extended_signal[i]),
            reinterpret_cast<const lv_32fc_t*>(coeffs.data()),
            static_cast<unsigned int>(coeff_len));
        filtered[i] = fc(reinterpret_cast<float*>(&dot)[0],
                         reinterpret_cast<float*>(&dot)[1]);
    }
}

void fir_filter(const signal_v& signal, const std::vector<float> coeffs,
                signal_v& filtered, const size_t size) {
    size_t coeff_len = coeffs.size();
    size_t delay = (coeff_len - 1) / 2;

    int ext_signal_len = size + delay * 2;
    signal_v extended_signal(ext_signal_len);

    for (size_t i = 0; i < delay; i++) {
        extended_signal[i] = signal[0];
    }

    std::copy(signal.begin(), signal.begin() + size,
              extended_signal.begin() + delay);

    for (size_t i = 0; i < delay; i++) {
        extended_signal[delay + size + i] = signal[size - 1];
    }

    for (size_t i = 0; i < size; ++i) {
        lv_32fc_t dot;
        volk_32fc_32f_dot_prod_32fc(
            &dot,
            reinterpret_cast<const lv_32fc_t*>(&extended_signal[i]),
            coeffs.data(),
            static_cast<unsigned int>(coeff_len));
        filtered[i] = fc(reinterpret_cast<float*>(&dot)[0],
                         reinterpret_cast<float*>(&dot)[1]);
    }
}

void downsample(const signal_v& src, signal_v& dest, const uint16_t down,
                const size_t size) {
    if (down == 1) {
        dest = src;
    } else {
        size_t new_size = size / down;
        for (size_t i = 0; i < new_size; ++i) {
            dest[i] = src[i * down];
        }
    }
}

void upsample(const signal_v& src, signal_v& dest, const uint16_t up,
              const size_t size) {
    if (up == 1) {
        dest = src;
    } else {
        for (size_t i = 0; i < size; i++) {
            dest[i * up] = src[i];
        }
    }
}

// Resample
void resample(const signal_v& src, signal_v& dest, const float& rate,
              const size_t size) {
    constexpr float halfband = 0.5, fractional_bw = 0.4;
    int up, down;
    float trans_width, cutoff;
    float beta = 8.6;  // 60 dB attenuation
    utils::computeResamplingFactors(rate, up, down);
    size_t new_size = size * up;
    signal_v temp_up(new_size);

    if (rate >= 1.0) {
        // trans_width is already normalized
        trans_width = halfband - fractional_bw;
        cutoff = halfband - trans_width / 2.0;
    } else {
        trans_width = rate * (halfband - fractional_bw);
        cutoff = rate * halfband - trans_width / 2.0;
    }

    // Reduction in taps to gain some performance
    int num_taps = utils::calcNTaps(rate, trans_width, beta) / 2;

    if (num_taps % 2 == 0) {
        num_taps++;
    }

    std::vector<float> taps =
        utils::lowpassKaiserWin(num_taps, cutoff, beta, up);

    std::reverse(taps.begin(), taps.end());

    upsample(src, temp_up, up, size);

    fir_filter(temp_up, taps, temp_up, new_size);

    downsample(temp_up, dest, down, new_size);
}

float calc_power(const signal_v& in1, const size_t size) {
    if (size == 0) return 0.0f;

    // Compute |z|^2 for each sample
    const size_t align = volk_get_alignment();
    float* mag_sq = static_cast<float*>(volk_malloc(size * sizeof(float), align));
    volk_32fc_magnitude_squared_32f(
        mag_sq,
        reinterpret_cast<const lv_32fc_t*>(in1.data()),
        static_cast<unsigned int>(size));

    // Accumulate
    float power = 0.0f;
    volk_32f_accumulator_s32f(&power, mag_sq, static_cast<unsigned int>(size));
    volk_free(mag_sq);

    return power / static_cast<float>(size);
}

float calc_peak_power(const signal_v& in1, const size_t size) {
    if (size == 0) return 0.0f;

    // Compute |z|^2 for each sample
    const size_t align = volk_get_alignment();
    float* mag_sq = static_cast<float*>(volk_malloc(size * sizeof(float), align));
    volk_32fc_magnitude_squared_32f(
        mag_sq,
        reinterpret_cast<const lv_32fc_t*>(in1.data()),
        static_cast<unsigned int>(size));

    // Find max
    uint32_t max_idx = 0;
    volk_32f_index_max_32u(&max_idx, mag_sq, static_cast<unsigned int>(size));
    float peak = mag_sq[max_idx];
    volk_free(mag_sq);

    return peak;
}

void apply_frequency_offset(signal_v& signal, size_t size,
                            double sample_rate_hz, double freq_offset_hz,
                            double& phase_rad) {
    if (size == 0) return;
    if (!std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0) return;
    if (!std::isfinite(freq_offset_hz) || freq_offset_hz == 0.0) return;

    constexpr double two_pi = 2.0 * static_cast<double>(PI);
    const double phase_inc = two_pi * freq_offset_hz / sample_rate_hz;

    double phase = std::remainder(phase_rad, two_pi);

    // VOLK rotator: applies e^{j*phase_inc} per sample via recurrence
    lv_32fc_t phase_inc_fc;
    reinterpret_cast<float*>(&phase_inc_fc)[0] =
        static_cast<float>(std::cos(phase_inc));
    reinterpret_cast<float*>(&phase_inc_fc)[1] =
        static_cast<float>(std::sin(phase_inc));

    lv_32fc_t initial_phase;
    reinterpret_cast<float*>(&initial_phase)[0] =
        static_cast<float>(std::cos(phase));
    reinterpret_cast<float*>(&initial_phase)[1] =
        static_cast<float>(std::sin(phase));

    volk_32fc_s32fc_x2_rotator_32fc(
        reinterpret_cast<lv_32fc_t*>(signal.data()),
        reinterpret_cast<const lv_32fc_t*>(signal.data()),
        phase_inc_fc,
        &initial_phase,
        static_cast<unsigned int>(size));

    // Update accumulated phase for continuity across buffers
    phase_rad = std::remainder(
        phase + phase_inc * static_cast<double>(size), two_pi);
}

}  // namespace signal
}  // namespace dsp
}  // namespace chem
