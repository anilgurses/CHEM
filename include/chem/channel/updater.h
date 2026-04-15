#pragma once

#include <string.h>

#include "../common.h"
#include "../nlohmann/json.hpp"
#include "spdlog/spdlog.h"

using json = nlohmann::json;

namespace chem {
namespace channel {
class Updater {
   public:
    Updater();
    ~Updater() = default;

    void Start();

   private:
    std::shared_ptr<spdlog::logger> logger;

    float m_sampleRate;
    float m_freq;
    bool m_mimo;
    uint8_t m_numChannels;
};
}  // namespace channel
}  // namespace chem
