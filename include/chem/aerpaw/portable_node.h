#pragma once

#include "../common.h"
#include "node_characteristics.h"

namespace chem::aerpaw::portable_node {

inline NodeCharacteristics characteristics() {
    NodeCharacteristics c;
    c.tx_pa_gain_db = 25;
    c.rx_gain_db = 19;
    c.tx_noise_figure_db = 4.1;
    c.rx_noise_figure_db = 1.45 + NOISE_FIGURE_RX;  // LNA + Receiver
    c.cable_loss_db = 1.0;
    c.source_power_dbfs = -12.0;
    c.tx_antennas = {"sa-1400-5900"};
    c.rx_antennas = {"sa-1400-5900"};
    return c;
}

}  // namespace chem::aerpaw::portable_node
