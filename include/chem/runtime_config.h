#pragma once

#include <atomic>

#include "chem/common.h"

namespace chem {

struct RuntimeConfig {
    // Channel update/coherency time in milliseconds.
    inline static std::atomic<int> channel_update_rate_ms{CH_UPD_RATE};

    // Vehicle position polling period in milliseconds (used to set MAVSDK
    // telemetry rate).
    inline static std::atomic<int> vehicle_poll_rate_ms{100};

    // Maximum number of CIR taps allowed per link (multipath model).
    inline static std::atomic<int> cir_max_taps{64};

    // Sandbox mode. Toggled by the
    // SANDBOX_MODE=1 environment variable or by explicit SANDBOX_* commands.
    inline static std::atomic<bool> sandbox_enabled{false};
    inline static std::atomic<int> sandbox_update_rate_ms{500};
};

}  // namespace chem
