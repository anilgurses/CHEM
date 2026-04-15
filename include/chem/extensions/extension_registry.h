#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "extension.h"

namespace chem {

class ExtensionRegistry {
   public:
    void registerExtension(std::unique_ptr<ChannelExtension> ext);

    ChannelExtension* get(const std::string& name) const;

    std::vector<std::string> list() const;

    void setContext(NodeMap& nodeMap, IntermediateMap& intermediateMap);

    void startFromConfig(const nlohmann::json& extensionsConfig);

    void stopAll();

    nlohmann::json routeCommand(const std::string& extensionName,
                                const std::string& action,
                                const nlohmann::json& params);

    nlohmann::json getAllStatus() const;

   private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::unique_ptr<ChannelExtension>> m_extensions;
    NodeMap* m_nodeMap{nullptr};
    IntermediateMap* m_intermediateMap{nullptr};
};

}  // namespace chem
