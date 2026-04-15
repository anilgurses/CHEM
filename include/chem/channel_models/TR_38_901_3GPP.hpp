#pragma once

#include <cmath>
#include <random>
#include <stdexcept>
#include <string>

#include "../dsp/channel.h"

namespace chem {
namespace channel_models {
namespace tr_38_901 {

// Scenarios
inline const std::string RMa = "RMa";
inline const std::string UMa = "UMa";
inline const std::string UMi_StreetCanyon = "UMi-StreetCanyon";

// Propagation condition
enum class PropagationCondition { LOS, NLOS };

struct LargeScaleParameters {
    double delay_spread;
    double k_factor;
    double shadowing;
};

inline PropagationCondition get_propagation_condition(
    const std::string& scenario, const float& dist, const float& ue_height) {
    // TR 38.901 Table 7.4.2-1: LOS probability as function of 2D distance
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    const float d2d = std::max(1.0f, dist);  // clamp to 1m minimum
    float p_los = 0.0f;

    if (scenario == RMa) {
        // P_LOS = exp(-(d2D - 10) / 1000)  for d2D > 10m, else 1
        if (d2d <= 10.0f) {
            p_los = 1.0f;
        } else {
            p_los = std::exp(-(d2d - 10.0f) / 1000.0f);
        }
    } else if (scenario == UMa) {
        // P_LOS = (min(18/d2D, 1) * (1 - exp(-d2D/63)) + exp(-d2D/63))
        if (d2d <= 18.0f) {
            p_los = 1.0f;
        } else {
            p_los = (18.0f / d2d) * (1.0f - std::exp(-d2d / 63.0f)) +
                    std::exp(-d2d / 63.0f);
        }
    } else if (scenario == UMi_StreetCanyon) {
        // P_LOS = (min(18/d2D, 1) * (1 - exp(-d2D/36)) + exp(-d2D/36))
        if (d2d <= 18.0f) {
            p_los = 1.0f;
        } else {
            p_los = (18.0f / d2d) * (1.0f - std::exp(-d2d / 36.0f)) +
                    std::exp(-d2d / 36.0f);
        }
    }

    return (uniform(rng) < p_los) ? PropagationCondition::LOS
                                  : PropagationCondition::NLOS;
}

inline double calculate_path_loss_rma(const float& dist_3d,
                                      const float& dist_2d,
                                      const float& freq_ghz,
                                      const float& ue_height,
                                      const float& bs_height,
                                      PropagationCondition prop_cond) {
    // TR 38.901 Table 7.4.1-1: RMa path loss
    // Default values per spec: W = 20m (street width), h = 5m (avg building
    // height)
    constexpr double W = 20.0;
    constexpr double h_avg = 5.0;
    const double fc = static_cast<double>(freq_ghz);
    const double hBS = static_cast<double>(std::max(1.0f, bs_height));
    const double hUT = static_cast<double>(std::max(1.0f, ue_height));
    const double d3D = static_cast<double>(std::max(1.0f, dist_3d));
    const double d2D = static_cast<double>(std::max(1.0f, dist_2d));

    double pl;
    if (prop_cond == PropagationCondition::LOS) {
        // Breakpoint distance: d_BP = 2*pi * hBS * hUT * fc / c  (fc in Hz)
        const double d_BP = 2.0 * PI * hBS * hUT * (fc * 1e9) / SPEED_OF_LIGHT;
        if (d2D <= d_BP) {
            // PL1 = 20*log10(40*pi*d3D*fc/3) + min(0.03*h^1.72, 10)*log10(d3D)
            //        - min(0.044*h^1.72, 14.77) + 0.002*log10(h)*d3D
            const double h172 = std::pow(h_avg, 1.72);
            pl = 20.0 * std::log10(40.0 * PI * d3D * fc / 3.0) +
                 std::min(0.03 * h172, 10.0) * std::log10(d3D) -
                 std::min(0.044 * h172, 14.77) +
                 0.002 * std::log10(h_avg) * d3D;
        } else {
            // PL2 = PL1(d_BP) + 40*log10(d3D / d_BP)
            const double d3D_BP =
                std::sqrt(d_BP * d_BP + std::pow(hBS - hUT, 2.0));
            const double h172 = std::pow(h_avg, 1.72);
            const double pl1_bp =
                20.0 * std::log10(40.0 * PI * d3D_BP * fc / 3.0) +
                std::min(0.03 * h172, 10.0) * std::log10(d3D_BP) -
                std::min(0.044 * h172, 14.77) +
                0.002 * std::log10(h_avg) * d3D_BP;
            pl = pl1_bp + 40.0 * std::log10(d3D / d3D_BP);
        }
    } else {  // NLOS
        // PL_RMa-NLOS = 161.04 - 7.1*log10(W) + 7.5*log10(h)
        //   - (24.37 - 3.7*(h/hBS)^2) * log10(hBS)
        //   + (43.42 - 3.1*log10(hBS)) * (log10(d3D) - 3)
        //   + 20*log10(fc) - (3.2*(log10(11.75*hUT))^2 - 4.97)
        const double pl_nlos =
            161.04 - 7.1 * std::log10(W) + 7.5 * std::log10(h_avg) -
            (24.37 - 3.7 * std::pow(h_avg / hBS, 2.0)) * std::log10(hBS) +
            (43.42 - 3.1 * std::log10(hBS)) * (std::log10(d3D) - 3.0) +
            20.0 * std::log10(fc) -
            (3.2 * std::pow(std::log10(11.75 * hUT), 2.0) - 4.97);
        // Take max of NLOS and LOS path loss
        const double pl_los =
            calculate_path_loss_rma(dist_3d, dist_2d, freq_ghz, ue_height,
                                    bs_height, PropagationCondition::LOS);
        pl = std::max(pl_nlos, pl_los);
    }
    return pl;
}

inline double calculate_path_loss_uma(const float& dist_3d,
                                      const float& freq_ghz,
                                      const float& ue_height,
                                      const float& bs_height,
                                      PropagationCondition prop_cond) {
    double pl;
    if (prop_cond == PropagationCondition::LOS) {
        pl = 28.0 + 22 * std::log10(LOG_GUARD(dist_3d)) +
             20 * std::log10(LOG_GUARD(freq_ghz));
    } else {  // NLOS
        pl = 13.54 + 39.08 * std::log10(LOG_GUARD(dist_3d)) +
             20 * std::log10(LOG_GUARD(freq_ghz)) - 0.6 * (ue_height - 1.5);
    }
    return pl;
}

inline double calculate_path_loss_umi(const float& dist_3d,
                                      const float& freq_ghz,
                                      const float& ue_height,
                                      const float& bs_height,
                                      PropagationCondition prop_cond) {
    double pl;
    if (prop_cond == PropagationCondition::LOS) {
        pl = 32.4 + 21 * std::log10(LOG_GUARD(dist_3d)) +
             20 * std::log10(LOG_GUARD(freq_ghz));
    } else {  // NLOS
        pl = 35.3 * std::log10(LOG_GUARD(dist_3d)) + 22.4 +
             21.3 * std::log10(LOG_GUARD(freq_ghz)) - 0.3 * (ue_height - 1.5);
    }
    return pl;
}

inline double calculate_path_loss(const std::string& scenario,
                                  const float& dist, const float& freq_ghz,
                                  const float& ue_height,
                                  const float& bs_height) {
    PropagationCondition prop_cond =
        get_propagation_condition(scenario, dist, ue_height);
    const float dist_3d =
        std::sqrt(dist * dist + std::pow(bs_height - ue_height, 2));

    if (scenario == RMa) {
        return calculate_path_loss_rma(dist_3d, dist, freq_ghz, ue_height,
                                       bs_height, prop_cond);
    } else if (scenario == UMa) {
        return calculate_path_loss_uma(dist_3d, freq_ghz, ue_height, bs_height,
                                       prop_cond);
    } else if (scenario == UMi_StreetCanyon) {
        return calculate_path_loss_umi(dist_3d, freq_ghz, ue_height, bs_height,
                                       prop_cond);
    } else {
        throw std::invalid_argument(
            "Unknown scenario for 3GPP TR 38.901 model");
    }
}

inline void apply_3gpp_38_901(const signal_v& in1,
                              signal_v& res, const float& dist,
                              const float& sig_power, const float& freq,
                              const float& ue_height, const float& bs_height,
                              const std::string& scenario, const size_t size,
                              const double& shadowing_std) {
    double path_loss_db =
        calculate_path_loss(scenario, dist, freq / 1e9, ue_height, bs_height);
    // Add shadowing
    std::random_device rd{};
    std::mt19937 gen{rd()};
    std::normal_distribution<> d{0, shadowing_std};
    path_loss_db += d(gen);

    double path_loss_lin = std::pow(10.0, -path_loss_db / 20.0);

    for (size_t i = 0; i < size; ++i) {
        res[i] = in1[i] * static_cast<float>(path_loss_lin);
    }
}

}  // namespace tr_38_901
}  // namespace channel_models
}  // namespace chem
