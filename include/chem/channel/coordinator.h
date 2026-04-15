#pragma once

#include <string.h>

#include <map>
#include <string>

#include "../common.h"
#include "../nlohmann/json.hpp"
#include "../node/node_entity.h"
#include "channel.h"
#include "chem/extensions/extension_registry.h"
#include "chem/net/tcp_server.h"
#include "intermediate.h"

namespace chem {
namespace channel {

class Coordinator : public std::enable_shared_from_this<Coordinator> {
   public:
    Coordinator(
        const std::string& ipv4addr, const uint16_t& portNum,
        std::map<std::string, chem::Node>& nodeMap,
        std::map<double, std::shared_ptr<chem::Intermediate>>& intermediate_map,
        std::shared_ptr<chem::PropagationDefaults> propagationDefaults =
            nullptr);
    ~Coordinator();

    void start();

    void requestHandler(const std::string& request, std::string& response);

    void updateChannels();

    void configChannels();

    std::string getChannels();

    std::string getNodes();

    std::string getIndividualChannels();

    std::string getAntennaPatterns();
    std::string getAntennaTypes();

    std::string getStatus();

    double calcAzimuth(const double& lat1, const double& lon1,
                       const double& lat2, const double& lon2);

    double calcDistance(const double& lat1, const double& lon1,
                        const double& lat2, const double& lon2);

    void setChannelCoeff();

    void setShadowingSTD();

    std::string setPathLossMode(const double& freq, const std::string plMode);
    std::string setPathLossMode(const double& freq, const std::string plMode,
                                float grCoeff);
    std::string setPathLossMode(const double& freq, const std::string plMode,
                                float grCoeff, const std::string& scenario);

    std::string setChCoeff(const double& freq, const std::string& src,
                           const std::string& dest, const struct chId& ch,
                           const double& coeff);

    std::string setNoiseModel(const double& freq, const std::string& src,
                              const std::string& dest,
                              const std::string noiseMdl, const double& snr);

    std::string setFrequencyOffset(const double& freq, const std::string& src,
                                   const std::string& dest,
                                   const double& freq_offset_hz);

    std::string setDopplerEnabled(const double& freq, const std::string& src,
                                  const std::string& dest, const bool& enabled);

    chem::ExtensionRegistry& getExtensionRegistry() {
        return m_extensionRegistry;
    }

    std::string setCIR(const double& freq, const std::string& src,
                       const std::string& dest, const struct chId& ch,
                       const chem::signal_v& taps);

   private:
    std::shared_ptr<spdlog::logger> logger;

    std::map<double, std::shared_ptr<chem::Intermediate>>& m_intermediateMap;
    std::map<std::string, Node>& m_nodeMap;
    std::shared_ptr<chem::PropagationDefaults> m_propagationDefaults;

    std::string m_ipAddress;
    uint16_t m_portNum;
    uint8_t m_numChannels;

    TCPServer m_tcpServer;

    chem::ExtensionRegistry m_extensionRegistry;
};
}  // namespace channel
}  // namespace chem
