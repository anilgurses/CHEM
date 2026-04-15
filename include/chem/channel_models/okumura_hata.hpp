#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "../common.h"

namespace chem {
namespace channel_models {
namespace okumura_hata {

enum class Environment { URBAN, SUBURBAN, OPEN };

inline Environment EnvironmentFromString(const std::string& env) {
    if (env == "URBAN") return Environment::URBAN;
    if (env == "SUBURBAN") return Environment::SUBURBAN;
    if (env == "OPEN") return Environment::OPEN;
    return Environment::URBAN;
}

inline std::string EnvironmentToString(Environment env) {
    switch (env) {
        case Environment::URBAN:
            return "URBAN";
        case Environment::SUBURBAN:
            return "SUBURBAN";
        case Environment::OPEN:
            return "OPEN";
        default:
            return "URBAN";
    }
}

// Small/medium city mobile antenna height correction factor a(hMS)
// Hata model for frequencies 150-1500 MHz
inline double mobile_correction(double freq_mhz, double h_ms) {
    // a(hMS) = (1.1*log10(fc) - 0.7)*hMS - (1.56*log10(fc) - 0.8)
    const double log_fc = std::log10(std::max(1.0, freq_mhz));
    return (1.1 * log_fc - 0.7) * h_ms - (1.56 * log_fc - 0.8);
}

// Core Hata urban formula
// freq_mhz: frequency in MHz (150-1500 MHz)
// dist_km: distance in km (1-20 km)
// h_bs: base station antenna height in meters (30-200m)
// h_ms: mobile station antenna height in meters (1-10m)
inline double calculate_urban(double freq_mhz, double dist_km, double h_bs,
                              double h_ms) {
    const double fc = std::clamp(freq_mhz, 150.0, 1500.0);
    const double d = std::max(0.1, dist_km);
    const double hb = std::clamp(h_bs, 1.0, 200.0);
    const double hm = std::clamp(h_ms, 1.0, 10.0);

    const double log_fc = std::log10(fc);
    const double a_hm = mobile_correction(fc, hm);

    // L_urban = 69.55 + 26.16*log10(fc) - 13.82*log10(hBS) - a(hMS)
    //           + (44.9 - 6.55*log10(hBS)) * log10(d)
    return 69.55 + 26.16 * log_fc - 13.82 * std::log10(hb) - a_hm +
           (44.9 - 6.55 * std::log10(hb)) * std::log10(d);
}

// Calculate path loss using Okumura-Hata model
// freq_hz: frequency in Hz
// dist_m: distance in meters
// h_bs: base station height in meters
// h_ms: mobile station height in meters
// env: environment type
inline double calculate_path_loss(double freq_hz, double dist_m, double h_bs,
                                  double h_ms, Environment env) {
    const double freq_mhz = freq_hz / 1.0e6;
    const double dist_km = dist_m / 1000.0;

    double pl = calculate_urban(freq_mhz, dist_km, h_bs, h_ms);

    switch (env) {
        case Environment::SUBURBAN: {
            // L_suburban = L_urban - 2*(log10(fc/28))^2 - 5.4
            const double log_ratio = std::log10(freq_mhz / 28.0);
            pl -= 2.0 * log_ratio * log_ratio + 5.4;
            break;
        }
        case Environment::OPEN: {
            // L_open = L_urban - 4.78*(log10(fc))^2 + 18.33*log10(fc) - 40.94
            const double log_fc = std::log10(freq_mhz);
            pl -= 4.78 * log_fc * log_fc - 18.33 * log_fc + 40.94;
            break;
        }
        case Environment::URBAN:
        default:
            break;
    }

    return pl;
}

}  // namespace okumura_hata
}  // namespace channel_models
}  // namespace chem
