#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chem/channel/intermediate.h"
#include "chem/dsp/channel.h"
#include "chem/extensions/extension.h"
#include "chem/node/node_entity.h"

namespace chem {

class SionnaExtension : public ChannelExtension {
   public:
    SionnaExtension() = default;
    ~SionnaExtension() override;

    // ChannelExtension interface
    std::string name() const override { return "sionna"; }
    bool onStart(const nlohmann::json& config) override;
    void onStop() override;
    bool isRunning() const override { return m_running.load(); }
    nlohmann::json handleCommand(const std::string& action,
                                 const nlohmann::json& params) override;
    nlohmann::json getStatus() const override;
    nlohmann::json getConfigSchema() const override;
    bool bypassesPathLoss() const override { return true; }

    const std::string& getServerUrl() const { return m_serverUrl; }
    const std::string& getSceneId() const { return m_sceneId; }

   private:
    void pollLoop();
    bool createScene();
    void syncPositions();
    void computeAndApplyCIR();

    // HTTP helpers
    std::string httpGet(const std::string& target);
    std::string httpPost(const std::string& target, const std::string& body);
    std::string httpPut(const std::string& target, const std::string& body);

    // Delay+gains -> FIR tap vector
    chem::signal_v cirToTaps(const std::vector<double>& delays,
                                            const std::vector<double>& gains_re,
                                            const std::vector<double>& gains_im,
                                            double sampleRate) const;

    void clearExtensionFlags();
    void parseUrl(const std::string& url);

    std::string m_serverUrl;
    std::string m_host;
    std::string m_port;
    double m_refLat{35.7272};
    double m_refLon{-78.6960};
    double m_refAlt{0.0};
    int m_updateRateMs{500};
    int m_maxDepth{3};
    int m_numSamples{100000};

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    // Sionna scene ID
    std::string m_sceneId;

    // Track which CHEM nodes are registered on Sionna
    // nodeId -> sionna name
    std::map<std::string, std::string> m_txMap;
    std::map<std::string, std::string> m_rxMap;

    // CIR index mapping
    std::vector<std::string> m_txOrder; 
    std::vector<std::string> m_rxOrder;

    // Cache last-sent positions to avoid redundant PUTs
    struct CachedPos {
        double lat, lon;
        float alt;
    };
    std::map<std::string, CachedPos> m_lastPositions;

    // CIR response time tracking (exponential moving average)
    double m_avgCirResponseMs{0.0};
    int m_cirResponseCount{0};

    std::shared_ptr<spdlog::logger> logger;
};

}  // namespace chem
