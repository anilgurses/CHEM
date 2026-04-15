#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include "chem/antennas/dipole.h"

namespace chem::antennas::generated {

struct PatternTable_SA_1400_5900 {
    float freq_mhz;
    float phi_min;
    float phi_step;
    size_t phi_size;
    float theta_min;
    float theta_step;
    size_t theta_size;
    const float* gains;  // row-major [theta][phi]
};

extern const PatternTable_SA_1400_5900 kTables_SA_1400_5900[14];
extern const size_t kTablesCount_SA_1400_5900;

const PatternTable_SA_1400_5900& nearest_table_SA_1400_5900(float freq_mhz);
float lookup_gain_SA_1400_5900(const PatternTable_SA_1400_5900& tbl, float theta, float phi);

class SA_1400_5900 : public Antenna {
public:
    explicit SA_1400_5900(double freq_hz) : freq_mhz_(static_cast<float>(freq_hz / 1e6)) {}
    double get_gain(double theta, double phi) const override {
        const auto& tbl = nearest_table_SA_1400_5900(freq_mhz_);
        return static_cast<double>(lookup_gain_SA_1400_5900(tbl, static_cast<float>(theta), static_cast<float>(phi)));
    }

private:
    float freq_mhz_;
};

} // namespace chem::antennas::generated
