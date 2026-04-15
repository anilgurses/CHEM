#include "chem/config.h"

#include <algorithm>
#include <fstream>
#include <iostream>

using namespace chem;

bool Config::read(std::string fpath) {
    spdlog::get("CHEM")->info("Parsing configuration");
    std::ifstream cfg(fpath);
    configuration = json::parse(cfg);

    try {
        controllerIpaddr = configuration["controllerIpAddress"];
        controllerPort = configuration["controllerPort"];
        coordIpaddress = configuration["coordIpaddress"];
        coordPort = configuration["coordPort"];
        maxNode = configuration["maxNode"];

        logDirectory = configuration["logDirectory"];
        logLevel = configuration["logLevel"];

        // Optional: CPU/NUMA affinity settings
        // Don't feel yourself obligated to use this if you are not running 
        // anything latency sensitive. I've observed that NUMA latency can go upto 200-300us between nodes
        if (configuration.contains("cpuAffinity")) {
            cpuAffinity = configuration["cpuAffinity"];
        }
        if (configuration.contains("numaNode")) {
            numaNode = configuration["numaNode"];
        }
        if (configuration.contains("maxCores")) {
            maxCores = std::max(1, static_cast<int>(configuration["maxCores"]));
        }
        if (configuration.contains("numaEnabled")) {
            numaEnabled = configuration["numaEnabled"].get<bool>();
        }

        // Extensions configuration
        if (configuration.contains("extensions") &&
            configuration["extensions"].is_object()) {
            extensionsConfig = configuration["extensions"];
        }
    } catch (const std::exception& e) {
        spdlog::error(e.what());
        return false;
    }

    return true;
}

const std::string& Config::getControllerIp() const { return controllerIpaddr; }

const uint16_t& Config::getControllerPort() const { return controllerPort; }

const std::string& Config::getCoordinatorIp() const { return coordIpaddress; }

const uint16_t& Config::getCoordinatorPort() const { return coordPort; }

const uint8_t& Config::getMaxNode() const { return maxNode; }

const std::string& Config::getLogDirectory() const { return logDirectory; }

const std::string& Config::getLogLevel() const { return logLevel; }

const std::string& Config::getCpuAffinity() const { return cpuAffinity; }

int Config::getNumaNode() const { return numaNode; }

int Config::getMaxCores() const { return maxCores; }

bool Config::getNumaEnabled() const { return numaEnabled; }

const nlohmann::json& Config::getExtensionsConfig() const {
    return extensionsConfig;
}
