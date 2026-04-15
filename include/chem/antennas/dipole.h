#pragma once

#include <cmath>

#include "../common.h"

namespace chem {
namespace antennas {

class Antenna {
   public:
    virtual ~Antenna() = default;
    /**
     * @brief Get the gain of the antenna in a specific direction.
     *
     * @param theta Elevation angle in radians.
     * @param phi Azimuth angle in radians.
     * @return double Gain in dBi.
     */
    virtual double get_gain(double theta, double phi) const = 0;
};

class IsotropicAntenna : public Antenna {
   public:
    double get_gain(double theta, double phi) const override {
        // Isotropic antenna has a gain of 1 (0 dBi) in all directions.
        return 0.0;
    }
};

class DipoleAntenna : public Antenna {
   private:
    double length;  // Length of the dipole in meters
    double freq;    // Frequency in Hz

   public:
    DipoleAntenna(double frequency) : freq(frequency) {
        // Half-wave dipole
        this->length = SPEED_OF_LIGHT / (2 * frequency);
    }

    double get_gain(double theta, double phi) const override {
        // The gain of a half-wave dipole antenna.
        // Assuming the dipole is aligned with the z-axis (vertical dipole).
        // theta is the angle from the z-axis (0 = up, 90 = horizon, 180 =
        // down). Maximum gain is at theta = 90° (broadside/horizon). Nulls are
        // at theta = 0° and 180° (along the dipole axis).
        constexpr double kMinSinTheta = 1e-6;
        const double sin_theta = std::sin(theta);
        if (std::abs(sin_theta) < kMinSinTheta) {
            return -60.0;  // Very low gain at the nulls (not -infinity for
                           // numerical stability)
        }
        const double pattern =
            std::cos((PI / 2.0) * std::cos(theta)) / sin_theta;
        const double gain_linear = pattern * pattern;
        // The directivity of a half-wave dipole is 1.64, which is 2.15 dBi
        return 10.0 * std::log10(gain_linear * 1.64);
    }
};

}  // namespace antennas
}  // namespace chem
