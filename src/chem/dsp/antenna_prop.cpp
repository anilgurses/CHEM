#include "chem/dsp/antenna_prop.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>

#include "chem/antennas/antennas.h"
#include "chem/common.h"
#include "chem/models/node_config.hpp"

// Some helper functions
double deg_to_rad(double deg) { return deg * (PI / 180.0); }

double wrap_degrees(double deg) {
    const double wrapped = std::fmod(deg, 360.0);
    return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
}

std::string normalize_antenna_name(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name;
}

std::string select_antenna_name(const chem::NodeConfig& config, bool is_tx) {
    // Priority 1: Check dynamically set antenna pattern from NodeConfig
    if (is_tx && config.hasTxAntennaPattern()) {
        const auto& pattern = config.getTxAntennaPattern();
        if (!pattern.empty()) return normalize_antenna_name(pattern);
    }
    if (!is_tx && config.hasRxAntennaPattern()) {
        const auto& pattern = config.getRxAntennaPattern();
        if (!pattern.empty()) return normalize_antenna_name(pattern);
    }

    // Priority 2: Check NodeCharacteristics (from AERPAW)
    if (config.hasCharacteristics()) {
        if (const auto& chars = config.getCharacteristics();
            chars.has_value()) {
            const auto& antennas =
                is_tx ? chars->tx_antennas : chars->rx_antennas;
            if (!antennas.empty())
                return normalize_antenna_name(antennas.front());
        }
    }

    // Priority 3: Check channel-level antenna settings
    for (const auto& ch : config.getChannels()) {
        const std::string name = is_tx ? ch.getTxAntenna() : ch.getRxAntenna();
        if (!name.empty()) return normalize_antenna_name(name);
    }
    // Default:
    return "isotropic";
}

namespace chem {
namespace dsp {
namespace antenna {

namespace {
struct AntennaCacheKey {
    std::string name;
    int64_t freq_khz;

    bool operator==(const AntennaCacheKey& other) const {
        return name == other.name && freq_khz == other.freq_khz;
    }
};

struct AntennaCacheKeyHash {
    size_t operator()(const AntennaCacheKey& k) const {
        return std::hash<std::string>{}(k.name) ^
               (std::hash<int64_t>{}(k.freq_khz) << 1);
    }
};

using AntennaCache =
    std::unordered_map<AntennaCacheKey,
                       std::shared_ptr<chem::antennas::Antenna>,
                       AntennaCacheKeyHash>;
}  // namespace

float compute_antenna_gain_db(const NodeConfig& config, bool is_tx,
                              double frequency_hz, float elevation_deg,
                              float azimuth_deg) {
    if (!std::isfinite(frequency_hz) || frequency_hz <= 0.0) return 0.0f;

    const std::string antenna_name = select_antenna_name(config, is_tx);

    static thread_local AntennaCache antenna_cache;
    AntennaCacheKey key{antenna_name,
                        static_cast<int64_t>(frequency_hz / 1000.0)};

    auto& cached = antenna_cache[key];
    if (!cached) {
        cached = chem::antennas::MakeAntenna(antenna_name, frequency_hz);
    }
    if (!cached) return 0.0f;

    const auto& antenna = cached;

    const double elev_deg =
        std::isfinite(elevation_deg) ? static_cast<double>(elevation_deg) : 0.0;
    const double az_deg =
        std::isfinite(azimuth_deg) ? static_cast<double>(azimuth_deg) : 0.0;

    const double theta_deg = std::clamp(90.0 - elev_deg, 0.0, 180.0);
    const double theta_rad = deg_to_rad(theta_deg);
    const double phi_rad =
        deg_to_rad(wrap_degrees(az_deg + (is_tx ? 0.0 : 180.0)));

    const float gain_db =
        static_cast<float>(antenna->get_gain(theta_rad, phi_rad));

    return gain_db;
}

}  // namespace antenna
}  // namespace dsp
}  // namespace chem
