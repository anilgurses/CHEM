#include "chem/extensions/extension_registry.h"

#include "chem/common.h"
#include "spdlog/fmt/fmt.h"

namespace chem {

void ExtensionRegistry::registerExtension(
    std::unique_ptr<ChannelExtension> ext) {
    if (!ext) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string n = ext->name();
    if (m_nodeMap && m_intermediateMap) {
        ext->setContext(*m_nodeMap, *m_intermediateMap);
    }
    m_extensions[n] = std::move(ext);
    LOG_INFO("EXTENSIONS", fmt::format("Registered extension '{}'", n));
}

ChannelExtension* ExtensionRegistry::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_extensions.find(name);
    if (it == m_extensions.end()) return nullptr;
    return it->second.get();
}

std::vector<std::string> ExtensionRegistry::list() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_extensions.size());
    for (const auto& [name, _] : m_extensions) {
        names.push_back(name);
    }
    return names;
}

void ExtensionRegistry::setContext(NodeMap& nodeMap,
                                   IntermediateMap& intermediateMap) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodeMap = &nodeMap;
    m_intermediateMap = &intermediateMap;
    for (auto& [_, ext] : m_extensions) {
        ext->setContext(nodeMap, intermediateMap);
    }
}

void ExtensionRegistry::startFromConfig(
    const nlohmann::json& extensionsConfig) {
    if (!extensionsConfig.is_object()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [name, cfg] : extensionsConfig.items()) {
        auto it = m_extensions.find(name);
        if (it == m_extensions.end()) {
            LOG_WARN("EXTENSIONS",
                     fmt::format("Extension '{}' not registered, skipping", name));
            continue;
        }
        bool enabled = true;
        if (cfg.is_object() && cfg.contains("enabled") &&
            cfg["enabled"].is_boolean()) {
            enabled = cfg["enabled"].get<bool>();
        }
        if (enabled) {
            LOG_INFO("EXTENSIONS",
                     fmt::format("Starting extension '{}' from config", name));
            it->second->onStart(cfg);
        }
    }
}

void ExtensionRegistry::stopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [name, ext] : m_extensions) {
        if (ext->isRunning()) {
            LOG_INFO("EXTENSIONS",
                     fmt::format("Stopping extension '{}'", name));
            ext->onStop();
        }
    }
}

nlohmann::json ExtensionRegistry::routeCommand(
    const std::string& extensionName, const std::string& action,
    const nlohmann::json& params) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_extensions.find(extensionName);
    if (it == m_extensions.end()) {
        return {{"status", "fail"},
                {"message", "Extension '" + extensionName + "' not found"}};
    }
    return it->second->handleCommand(action, params);
}

nlohmann::json ExtensionRegistry::getAllStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [name, ext] : m_extensions) {
        auto status = ext->getStatus();
        auto schema = ext->getConfigSchema();
        if (!schema.empty()) {
            status["configSchema"] = schema;
        }
        result[name] = status;
    }
    return result;
}

}  // namespace chem
