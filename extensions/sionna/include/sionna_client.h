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
    void fetchSceneAlignment();
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

    double destSampleRate(const std::string& destId, double fallback) const;

    std::string m_serverUrl;
    std::string m_host;
    std::string m_port;

    std::string m_sceneConfig{"aerpaw"};
    // scene_origin: geo reference. alt is the ellipsoidal height (HAE) of
    // ground level; scene z=0 is this ground.
    double m_refLat{35.72750947};
    double m_refLon{-78.69595819};
    double m_refAlt{112.0};
    // scene_offset (meters): scene_xyz = ENU_meters(origin) * scale + offset.
    double m_offsetX{118.1};
    double m_offsetY{-123.4};
    double m_offsetZ{0.0};
    // scale: 1.0 because the aerpaw scene is already in meters.
    double m_scale{1.0};

    nlohmann::json m_sceneAlignment;

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
