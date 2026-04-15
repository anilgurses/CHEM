#pragma once

#include <map>
#include <memory>
#include <string>

#include "../nlohmann/json.hpp"
#include "../node/node_entity.h"
#include "../channel/intermediate.h"

namespace chem {

using NodeMap = std::map<std::string, chem::Node>;
using IntermediateMap = std::map<double, std::shared_ptr<chem::Intermediate>>;

class ChannelExtension {
   public:
    virtual ~ChannelExtension() = default;

    virtual std::string name() const = 0;

    virtual bool onStart(const nlohmann::json& config) = 0;

    virtual void onStop() = 0;

    virtual bool isRunning() const = 0;

    virtual nlohmann::json handleCommand(const std::string& action,
                                         const nlohmann::json& params) = 0;

    virtual nlohmann::json getStatus() const = 0;

    virtual nlohmann::json getConfigSchema() const {
        return nlohmann::json::object();
    }

    // Returns true if this extension replaces statistical propagation
    // models (in channel/channel.cpp) (FSPL, Two-Ray, 3GPP, etc.) with its own path loss computation.
    // The extension can provide loss via CIR taps, via setExtensionPathLossDb()
    // on individual channels, or both.
    // Extensions that add impairments on top of existing models return false.
    virtual bool bypassesPathLoss() const { return false; }

    void setContext(NodeMap& nodeMap, IntermediateMap& intermediateMap) {
        m_nodeMap = &nodeMap;
        m_intermediateMap = &intermediateMap;
    }

   protected:
    NodeMap* m_nodeMap{nullptr};
    IntermediateMap* m_intermediateMap{nullptr};
};

}  // namespace chem
