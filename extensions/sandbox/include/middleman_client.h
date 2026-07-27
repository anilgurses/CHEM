#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "chem/extensions/extension.h"

namespace chem {

// Sandbox mode 
//
// When active, CHEM does not receive IQ samples. A real USRP is
// driving the air interface through an external RF channel emulator
// (e.g. Propsim), and the "middleman" service is responsible for applying
// the CHEM-computed path loss to the hardware loop. This extension
// periodically POSTs the current per-link path loss, distance, and elevation
// to the middleman HTTP endpoint.
class SandboxExtension : public ChannelExtension {
   public:
    SandboxExtension() = default;
    ~SandboxExtension() override;

    // ChannelExtension interface
    std::string name() const override { return "sandbox"; }
    bool onStart(const nlohmann::json& config) override;
    void onStop() override;
    bool isRunning() const override { return m_running.load(); }
    nlohmann::json handleCommand(const std::string& action,
                                 const nlohmann::json& params) override;
    nlohmann::json getStatus() const override;
    nlohmann::json getConfigSchema() const override;

    const std::string& getServerUrl() const { return m_serverUrl; }

   private:
    void pollLoop();
    void pushPathLoss();

    std::string httpPost(const std::string& target, const std::string& body);

    void parseUrl(const std::string& url);

    std::string m_serverUrl;
    std::string m_host;
    std::string m_port;

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    std::shared_ptr<spdlog::logger> logger;
};

}  // namespace chem
