#pragma once

#include "../common.h"
#include "node_characteristics.h"

namespace chem::aerpaw::fixed_node {

inline NodeCharacteristics characteristics() {
    NodeCharacteristics c;
    c.tx_pa_gain_db = 30;
    c.rx_gain_db = 19;  // From LNA
    c.tx_noise_figure_db = 5;
    c.rx_noise_figure_db = 1.45 + NOISE_FIGURE_RX;  // LNA + Receiver
    // 5 dB from interconnect (At least that's what is measured/reported in
    // AERPAW RF frontend spreadsheet) + 0.5 dB from connectors
    c.cable_loss_db = 5;
    c.source_power_dbfs = -12.0;
    c.tx_antennas = {"rm-wb1-dn-blk right side up"};
    c.rx_antennas = {"rm-wb1-dn-blk right side up"};
    return c;
}

}  // namespace chem::aerpaw::fixed_node
