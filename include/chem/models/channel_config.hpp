#pragma once

#include <string.h>

#include <nlohmann/json.hpp>

#include "../common.h"
#include "chem/channel/channel.h"

using json = nlohmann::json;

namespace chem {
class ChannelConfig {
   public:
    ChannelConfig() : logger(spdlog::get("CHEM")) {};
    ChannelConfig(std::vector<Channel> chList, std::vector<json> chPaths,
                  std::vector<float> pDelays)
        : channelList(chList), channelPaths(chPaths), pathDelays(pDelays) {};

    void setChannel(std::vector<Channel> chList) { channelList = chList; }
    std::vector<Channel> getChannelList() const { return channelList; }

    void setChannelPaths(std::vector<json> chPaths) { channelPaths = chPaths; }
    std::vector<json> getChannelPaths() { return channelPaths; }

    void setPathDelays(std::vector<float> pDelays) { pathDelays = pDelays; }
    std::vector<float> getPathDelays() { return pathDelays; }

    bool parseInfo(json info) {
        try {
            pathDelays = info["pathDelays"].get<std::vector<float>>();
            channelPaths = info["channelPaths"].get<std::vector<json>>();

        } catch (std::exception& e) {
            logger->error("Error occured while parsing the info. Cause : {}",
                          e.what());
            return false;
        }
        return true;
    }

   private:
    std::vector<Channel> channelList;
    std::vector<json> channelPaths;
    std::vector<float> pathDelays;
};
}  // namespace chem
