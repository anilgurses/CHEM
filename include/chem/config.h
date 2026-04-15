#pragma once

#include <string>

#include "nlohmann/json.hpp"
#include "spdlog/spdlog.h"

using json = nlohmann::json;

namespace chem {
class Config {
   public:
    Config() = default;
    ~Config() = default;

    bool read(std::string fpath);

    const std::string& getControllerIp() const;
    const uint16_t& getControllerPort() const;

    const std::string& getCoordinatorIp() const;
    const uint16_t& getCoordinatorPort() const;

    const uint8_t& getMaxNode() const;

    const std::string& getLogDirectory() const;
    const std::string& getLogLevel() const;

    // CPU/NUMA affinity settings
    const std::string& getCpuAffinity() const;
    int getNumaNode() const;
    int getMaxCores() const;
    bool getNumaEnabled() const;

    // Generic extensions config 
    const nlohmann::json& getExtensionsConfig() const;

   private:
    json configuration;

    std::string controllerIpaddr = "0.0.0.0";
    uint16_t controllerPort = 10000;
    std::string coordIpaddress = "0.0.0.0";
    uint16_t coordPort = 5000;
    uint8_t maxNode = 2;

    std::string logDirectory = "chem.log";
    std::string logLevel = "info";

    // CPU/NUMA affinity (empty = no binding, -1 = no NUMA binding)
    std::string cpuAffinity = "";
    int numaNode = -1;
    int maxCores = 10;
    bool numaEnabled = true;

    // Extensions configuration 
    nlohmann::json extensionsConfig = nlohmann::json::object();
};
}  // namespace chem
