#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include "chem/antennas/dipole.h"

namespace chem::antennas::generated {

struct PatternTable_RM_WB1_DN_BLK_Upside_Down {
    float freq_mhz;
    float phi_min;
    float phi_step;
    size_t phi_size;
    float theta_min;
    float theta_step;
    size_t theta_size;
    const float* gains;  // row-major [theta][phi]
};

extern const PatternTable_RM_WB1_DN_BLK_Upside_Down kTables_RM_WB1_DN_BLK_Upside_Down[14];
extern const size_t kTablesCount_RM_WB1_DN_BLK_Upside_Down;

const PatternTable_RM_WB1_DN_BLK_Upside_Down& nearest_table_RM_WB1_DN_BLK_Upside_Down(float freq_mhz);
float lookup_gain_RM_WB1_DN_BLK_Upside_Down(const PatternTable_RM_WB1_DN_BLK_Upside_Down& tbl, float theta, float phi);

class RM_WB1_DN_BLK_Upside_Down : public Antenna {
public:
    explicit RM_WB1_DN_BLK_Upside_Down(double freq_hz) : freq_mhz_(static_cast<float>(freq_hz / 1e6)) {}
    double get_gain(double theta, double phi) const override {
        const auto& tbl = nearest_table_RM_WB1_DN_BLK_Upside_Down(freq_mhz_);
        return static_cast<double>(lookup_gain_RM_WB1_DN_BLK_Upside_Down(tbl, static_cast<float>(theta), static_cast<float>(phi)));
    }

private:
    float freq_mhz_;
};

} // namespace chem::antennas::generated
