#pragma once

#include <string>

namespace chem {
class NodeConfig;
}

namespace chem {
namespace dsp {
namespace antenna {

// Compute antenna gain (dBi) for a node given look direction and frequency.
float compute_antenna_gain_db(const NodeConfig& config, bool is_tx,
                              double frequency_hz, float elevation_deg,
                              float azimuth_deg);

}  // namespace antenna
}  // namespace dsp
}  // namespace chem
