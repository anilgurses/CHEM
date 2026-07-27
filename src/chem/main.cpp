/* main.cpp
 *
 * Copyright (C) 2024 Anil Gurses
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
 * @date 09/15/22
 *
 */

#include <tbb/global_control.h>

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/version.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#include "chem/channel/coordinator.h"
#include "chem/common.h"
#include "chem/config.h"
#include "chem/db/dbHandler.h"
#include "chem/logger.hpp"
#include "chem/node/controller.h"
#include "chem/numa_utils.h"
#include "chem/runtime_config.h"
#include "spdlog/spdlog.h"

using std::chrono::seconds;
using namespace boost::asio;

int main(int argc, char** argv) {
    chem::Config chemCfg;
    const char* homeDir = std::getenv("HOME");
    std::string fpath = std::string(homeDir) + "/.config/CHEM/config.json";

    if (argc > 1) {
        fpath = argv[1];
        if (fpath.find(".json") == std::string::npos) {
            spdlog::error("Configuration file should be json. Given path : {}",
                          fpath);
            return 1;
        }
    }
    const bool cfgOk = chemCfg.read(fpath);

    chem::Logger _logger(chemCfg.getLogDirectory());

    std::shared_ptr<spdlog::logger> logger = spdlog::get("CHEM");

    // TODO: Move to common.h
    std::cout << "\x1B[32m" << ACHEM_OPENING_MSG << "\x1B[0m\n" << std::endl;

    logger->info(fmt::format("Version {}", ACHEM_VERSION));

    if (cfgOk) logger->debug("Parsed the configuration!");

    logger->set_level(spdlog::level::from_str(chemCfg.getLogLevel()));

    // AP_EXPENV_SESSION_ENV=Sandbox overrides config; real RF is driven by a USRP and an
    // external channel emulator (Propsim in our use case). CHEM only computes path loss and
    // antenna patterns; no IQ is moved inside CHEM.
    const char* sandboxEnv = std::getenv("AP_EXPENV_SESSION_ENV");
    const bool sandboxEnabled =
        (sandboxEnv && std::string(sandboxEnv) == "Sandbox") ||
        chemCfg.getSandboxEnabled();
    if (sandboxEnabled) {
        chem::RuntimeConfig::sandbox_enabled.store(true);
        chem::RuntimeConfig::sandbox_update_rate_ms.store(
            chemCfg.getSandboxUpdateRateMs());
        std::cout << "\x1B[1;31m"
                  << "====================================\n"
                  << "  CHEM is running in SANDBOX mode\n"
                  << "  [ NO IQ TRANSMISSION ]\n"
                  << "===================================="
                  << "\x1B[0m" << std::endl;
    }

#ifdef ENABLE_PROFILING
    LOG_WARN(
        "MAIN",
        "PROFILING MODE ENABLED - Performance profiling active, CSV output: "
        "chem_profiling.csv");
#endif

    LOG_INFO("MAIN", fmt::format("Log level -> {}", chemCfg.getLogLevel()));

    const int max_cores = std::max(1, chemCfg.getMaxCores());

    // Limit oneTBB worker parallelism used by channel processing paths.
    auto tbb_parallelism_guard = std::make_unique<tbb::global_control>(
        tbb::global_control::max_allowed_parallelism, max_cores);
    LOG_DEBUG("MAIN", fmt::format("TBB max parallelism set to {}.", max_cores));

    std::string dbPath = std::string(homeDir) + "/.config/CHEM/chem.db3";
    auto* db = chem::DBHandler::GetInstance(dbPath);

    db->CreateTables();
    LOG_INFO("DB", fmt::format("DB path: {}", dbPath));

    // NUMA initialization
    int selected_numa_node = 0;
    bool numa_active = false;
    if (chemCfg.getNumaEnabled() && chem::numa::available()) {
        selected_numa_node = (chemCfg.getNumaNode() >= 0)
                                 ? chemCfg.getNumaNode()
                                 : chem::numa::pick_least_loaded_node();
        chem::numa::bind_process_to_node(selected_numa_node);
        numa_active = true;
        LOG_INFO("MAIN",
                 fmt::format("NUMA: Bound to node {} ({} nodes available)",
                             selected_numa_node, chem::numa::node_count()));
    } else if (!chemCfg.getNumaEnabled()) {
        LOG_INFO("MAIN", "NUMA: Disabled by configuration");
    } else {
        LOG_INFO("MAIN", "NUMA: Not available on this system");
    }

    std::map<double, std::shared_ptr<chem::Intermediate>> intermediateMap;
    std::map<std::string, chem::Node> nodeList;

    auto propagationDefaults = std::make_shared<chem::PropagationDefaults>();

    auto controller = std::make_shared<chem::emulator::Controller>(
        chemCfg.getControllerIp(), chemCfg.getControllerPort(), nodeList,
        intermediateMap, propagationDefaults, selected_numa_node, numa_active);

    auto coordinator = std::make_shared<chem::channel::Coordinator>(
        chemCfg.getCoordinatorIp(), chemCfg.getCoordinatorPort(), nodeList,
        intermediateMap, propagationDefaults, &chemCfg);

    // Start extensions from config file
    coordinator->getExtensionRegistry().startFromConfig(
        chemCfg.getExtensionsConfig());

    // SANDBOX_MODE env var forces the sandbox extension on regardless of
    // config, and pushes path loss to the middleman URL from config.json.
    if (sandboxEnabled) {
        nlohmann::json sandboxCfg;
        sandboxCfg["middlemanUrl"] = chemCfg.getSandboxMiddlemanUrl();
        sandboxCfg["updateRateMs"] = chemCfg.getSandboxUpdateRateMs();
        coordinator->getExtensionRegistry().routeCommand(
            "sandbox", "start", sandboxCfg);
    }

    // Start Channel Coordinator
    std::thread coord_th(&chem::channel::Coordinator::start, coordinator);
    // Start Emulator Controller
    std::thread cntrl_th(&chem::emulator::Controller::Start, controller);

    coord_th.join();
    cntrl_th.join();

    return 0;
}
