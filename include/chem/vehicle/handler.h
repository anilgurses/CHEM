/* handler.h
 *
 * Copyright (C) 2022 Anil Gurses
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * @author Anil Gurses <agurses@ncsu.edu>
 */

#pragma once

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "../common.h"
#include "../nlohmann/json.hpp"
#include "../node/node_entity.h"
#include "spdlog/spdlog.h"

using namespace mavsdk;

using json = nlohmann::json;

namespace chem {
namespace vehicle {
class Handler {
   public:
    Handler(std::string connection, std::string nodeId, chem::Node& node);
    ~Handler();

    bool Connect();
    void Start();

    const double getLat() const;
    const double getLon() const;
    const float getAlt() const;

    bool isRunning();
    void Stop() { _running = false; }

    // void getSpeed();
    // void getType();

   private:
    std::string m_NodeId;
    std::string connection_str;

    chem::Node* m_node;

    float relative_alt = 0;
    double lat;
    double lon;

    std::atomic<bool> _running{false};

    std::chrono::steady_clock::time_point m_lastPositionUpdate{};
    std::chrono::steady_clock::time_point m_lastVelocityUpdate{};

    std::shared_ptr<System> m_system;
    Mavsdk mavsdk;
};
}  // namespace vehicle
}  // namespace chem
