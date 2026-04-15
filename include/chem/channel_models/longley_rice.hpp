#pragma once

#include <algorithm>
#include <cmath>

#include "../common.h"

namespace chem {
namespace channel_models {
namespace longley_rice {

// ITM Climate zones (per NTIA Report 82-100)
enum class Climate : int {
    EQUATORIAL = 1,
    CONTINENTAL_SUBTROPICAL = 2,
    MARITIME_SUBTROPICAL = 3,
    DESERT = 4,
    CONTINENTAL_TEMPERATE = 5,
    MARITIME_TEMPERATE_OVER_LAND = 6,
    MARITIME_TEMPERATE_OVER_SEA = 7
};

struct ITMParams {
    double freq_hz{915.0e6};
    double tx_height_m{10.0};
    double rx_height_m{1.5};
    double refractivity{301.0};         // N-units (surface refractivity)
    double ground_conductivity{0.005};  // S/m
    double ground_permittivity{15.0};   // relative
    Climate climate{Climate::CONTINENTAL_TEMPERATE};
};

// Effective earth radius factor from surface refractivity
// k = 1 / (1 - 0.04665 * exp(0.005577 * N_s))
inline double effective_earth_radius_factor(double N_s) {
    return 1.0 / (1.0 - 0.04665 * std::exp(0.005577 * N_s));
}

// Smooth-earth radio horizon distance for antenna height h (meters)
// d_h = sqrt(2 * k * a_e * h)   where a_e = EARTH_RADIUS
inline double horizon_distance(double h, double k_factor) {
    return std::sqrt(2.0 * k_factor * static_cast<double>(EARTH_RADIUS) *
                     std::max(0.01, h));
}

// Fresnel-Kirchhoff single-knife-edge diffraction loss (dB)
// v = Fresnel parameter
inline double knife_edge_loss(double v) {
    if (v <= -0.78) {
        return 0.0;
    }
    // Approximation: J(v) = 6.02 + 9.11*v - 1.27*v^2  for v > -0.78
    // More accurate piecewise from ITM:
    if (v < 0.0) {
        return 6.02 + 9.11 * v - 1.27 * v * v;
    }
    // For v >= 0: 6.02 + 9.0*v + 1.65*v^2
    return 6.02 + 9.0 * v + 1.65 * v * v;
}

// Diffraction loss for smooth earth beyond-horizon path
// Uses simplified Fresnel parameter based on path geometry
inline double diffraction_loss(double dist_m, double freq_hz,
                               const ITMParams& params, double k_factor) {
    const double lambda = static_cast<double>(SPEED_OF_LIGHT) / freq_hz;
    const double a_e = k_factor * static_cast<double>(EARTH_RADIUS);

    const double d_lt = horizon_distance(params.tx_height_m, k_factor);
    const double d_lr = horizon_distance(params.rx_height_m, k_factor);
    const double d_los = d_lt + d_lr;  // total LOS horizon distance

    if (dist_m <= d_los) {
        return 0.0;  // Within LOS region
    }

    // Distance beyond horizon
    const double d_beyond = dist_m - d_los;

    // Effective obstacle height at the horizon tangent point
    // h_eff ~ d_beyond^2 / (2 * a_e)  for smooth earth
    const double h_eff = (d_beyond * d_beyond) / (2.0 * a_e);

    // Fresnel parameter: v = h_eff * sqrt(2 / (lambda * d1 * d2 / (d1+d2)))
    // Simplified: use d_beyond/2 as effective obstacle distances
    const double d1 = d_los;
    const double d2 = d_beyond;
    const double d_eff = (d1 * d2) / std::max(1.0, d1 + d2);
    const double v = h_eff * std::sqrt(2.0 / (lambda * d_eff));

    return knife_edge_loss(v);
}

// Simplified troposcatter loss (dB) based on Yeh model
// Used for paths well beyond the radio horizon
inline double troposcatter_loss(double dist_m, double freq_hz,
                                const ITMParams& params, double k_factor) {
    const double freq_mhz = freq_hz / 1.0e6;
    const double dist_km = dist_m / 1000.0;

    // Angular distance (radians) for smooth earth
    const double a_e = k_factor * static_cast<double>(EARTH_RADIUS);
    const double theta_s = dist_m / a_e -
                           params.tx_height_m / std::max(1.0, dist_m) -
                           params.rx_height_m / std::max(1.0, dist_m);

    if (theta_s <= 0.0) {
        return 0.0;
    }

    // Simplified Yeh troposcatter formula:
    // L_s = 190.1 + 20*log10(freq_ghz) + 20*log10(dist_km) + 0.573*theta_deg -
    // 0.15*N_s
    const double theta_deg = theta_s * (180.0 / PI);
    const double freq_ghz = freq_mhz / 1000.0;

    return 190.1 + 20.0 * std::log10(std::max(0.001, freq_ghz)) +
           20.0 * std::log10(std::max(0.01, dist_km)) + 0.573 * theta_deg -
           0.15 * params.refractivity;
}

// Main path loss calculation
// Returns path loss in dB
inline double calculate_path_loss(double dist_m, const ITMParams& params) {
    const double d = std::max(1.0, dist_m);
    const double freq = std::max(1.0e6, params.freq_hz);
    const double k_factor = effective_earth_radius_factor(params.refractivity);

    // Free space path loss (FSPL) baseline
    const double fspl =
        20.0 * std::log10(d) + 20.0 * std::log10(freq) + FSPL_CONSTANT;

    // Horizon distances
    const double d_lt = horizon_distance(params.tx_height_m, k_factor);
    const double d_lr = horizon_distance(params.rx_height_m, k_factor);
    const double d_los = d_lt + d_lr;

    if (d <= d_los) {
        // Within line-of-sight: use FSPL
        return fspl;
    }

    // Beyond horizon: FSPL + diffraction and/or troposcatter
    const double l_diff = diffraction_loss(d, freq, params, k_factor);
    const double l_tropo = troposcatter_loss(d, freq, params, k_factor);

    // ITM blends diffraction and troposcatter; for simplification,
    // use the lesser of the two (dominant mode)
    const double beyond_loss = std::min(l_diff, l_tropo);

    return fspl + beyond_loss;
}

}  // namespace longley_rice
}  // namespace channel_models
}  // namespace chem
