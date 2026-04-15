#pragma once

#include <limits>
#include <string>
#include <vector>

namespace chem::aerpaw {

struct NodeCharacteristics {
    double tx_pa_gain_db = 0.0;
    double rx_gain_db = 0.0;
    double tx_noise_figure_db = 0.0;
    double rx_noise_figure_db = 0.0;
    double cable_loss_db = 0.0;
    double source_power_dbfs =
        std::numeric_limits<double>::quiet_NaN();  // NaN = unset (use measured)
    std::vector<std::string> tx_antennas;
    std::vector<std::string> rx_antennas;
};

}  // namespace chem::aerpaw
