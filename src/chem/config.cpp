#include "chem/config.h"

#include <algorithm>
#include <fstream>
#include <iostream>

using namespace chem;

bool Config::read(std::string fpath) {
    spdlog::info("Parsing configuration");
    m_path = fpath;
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

        // Optional: Sandbox mode
        if (configuration.contains("sandbox") &&
            configuration["sandbox"].is_object()) {
            const auto& sb = configuration["sandbox"];
            if (sb.contains("enabled") && sb["enabled"].is_boolean())
                sandboxEnabled = sb["enabled"];
            if (sb.contains("middlemanUrl") && sb["middlemanUrl"].is_string())
                sandboxMiddlemanUrl = sb["middlemanUrl"];
            if (sb.contains("updateRateMs") && sb["updateRateMs"].is_number())
                sandboxUpdateRateMs =
                    std::max(50, sb["updateRateMs"].get<int>());
        }
    } catch (const std::exception& e) {
        spdlog::error(e.what());
        return false;
    }

    return true;
}

bool Config::save(const std::string& path) const {
    const std::string target = path.empty() ? m_path : path;
    if (target.empty()) {
        spdlog::error("Config::save called with no path");
        return false;
    }
    std::ofstream out(target, std::ios::trunc);
    if (!out) {
        spdlog::error("Could not open config file for writing: {}", target);
        return false;
    }
    out << configuration.dump(4) << '\n';
    return out.good();
}

void Config::mergePatch(const nlohmann::json& patch) {
    configuration.merge_patch(patch);
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

bool Config::getSandboxEnabled() const { return sandboxEnabled; }

const std::string& Config::getSandboxMiddlemanUrl() const {
    return sandboxMiddlemanUrl;
}

int Config::getSandboxUpdateRateMs() const { return sandboxUpdateRateMs; }
