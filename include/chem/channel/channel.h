#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "../common.h"
#include "../dsp/channel.h"

// Make src and dest union
struct chId {
    size_t src;
    size_t dest;

    bool operator==(const chId& ch) const {
        return (ch.src == src) && (ch.dest == dest);
    }

    bool operator<(const chId& ch) const {
        return std::tie(src, dest) < std::tie(ch.src, ch.dest);
    }

    bool operator>(const chId& ch) const {
        return std::tie(src, dest) > std::tie(ch.src, ch.dest);
    }
};

namespace chem {
class NodeConfig;

struct LinkBudget {
    float tx_gain_db{0.0f};
    float rx_gain_db{0.0f};
    double noise_figure_db{0.0};
    float tx_ant_gain_db{0.0f};
    float rx_ant_gain_db{0.0f};
    float tx_cable_loss_db{0.0f};
    float rx_cable_loss_db{0.0f};
    double source_power_dbfs{std::numeric_limits<double>::quiet_NaN()};
};

/**
 * @brief Timing data for each channel impairment step in processChannel()
 *
 * Used for detailed benchmarking to measure latency of each processing step.
 */
struct ChannelProcessTiming {
    int64_t t_start{0};        // Before any processing
    int64_t t_resample{0};     // After resampling/gain scaling
    int64_t t_cir{0};          // After CIR/multipath application
    int64_t t_pathloss{0};     // After path loss calculation
    int64_t t_noise{0};        // After AWGN noise addition
    int64_t t_freq_offset{0};  // After frequency offset/Doppler
    int64_t t_end{0};          // End of processing
};

struct ChannelProcessParams {
    float coeff;
    size_t s_perBuff;
    float sample_ratio;
    double sample_rate_hz;
    size_t src_channel_index;
    size_t dest_channel_index;
    PropagationModel propagationModel;
    float ground_coeff;
    const NodeConfig* src_config;
    const NodeConfig* dest_config;
    double freq;
    ChannelProcessTiming* timing{
        nullptr};  // Optional timing collection (nullptr = disabled)
};

class Channel {
   public:
    Channel(std::string source, std::string dest, uint8_t txNumCh,
            uint8_t rxNumCh);

    // TODO add calculations under these functions
    void updateDistance(const float& distance);

    void updateAltitude(const float& alt);
    void updateHeights(const float& tx_height, const float& rx_height);
    void update3gppScenario(std::string scenario);
    void updateHataEnvironment(std::string environment);
    void updateITMParams(float refractivity, float ground_conductivity,
                         float ground_permittivity, int climate_zone);

    void updateElevation(const float& elevation);

    void updateAzimuth(const float& azimuth);

    float getLinkDistance() const;
    float getLinkAltitude() const { return _linkInfo.h_dist; }
    float getLinkElevation() const { return _linkInfo.elevation; }
    float getLinkAzimuth() const { return _linkInfo.azimuth; }

    void updateNoiseType(chem::NoiseType noise);

    chem::NoiseType getNoiseType() const;

    void updatePlType(chem::PathLossType pl);

    chem::PathLossType getPlType() const;
    void setPl(float pl_db);
    float getPlDb() const;

    std::string getSrc() const;

    std::string getDest() const;

    // TODO remove "s"
    float getChCoeffs(const uint8_t& src, const uint8_t& dest);

    void updateChCoeff(const chId& ch, const float& val);

    std::map<chId, float> getIndChannels() const;

    void setFrequencyOffsetHz(double offset_hz);
    double getFrequencyOffsetHz() const;
    void setDopplerEnabled(bool enabled);
    bool isDopplerEnabled() const;
    void setDopplerHz(double doppler_hz);
    double getDopplerHz() const;
    void applyFrequencyOffset(signal_v& signal, size_t size,
                              double sample_rate_hz, size_t dest_channel_index);

    /**
     * @brief Process channel impairments on signal
     * @param src Source signal buffer
     * @param dest Destination signal buffer
     * @param params Channel processing parameters (includes optional timing
     * pointer)
     *
     */
    void processChannel(signal_v& src,
                        signal_v& dest,
                        const ChannelProcessParams& params);

    void updateSNR(const float& snr);
    void clearSNROverride();
    bool hasSNROverride() const;
    float getSNR() const;
    float getMeasuredSNRdB() const;
    void setShadowingSTD(double std);

    void setCIRApplyMethod(dsp::channel::CIRApplyMethod method);
    dsp::channel::CIRApplyMethod getCIRApplyMethod() const;
    void updateChTaps(const chId& ch, const signal_v& taps);
    void clearChTaps(const chId& ch);
    size_t getChTapCount(const chId& ch) const;

    void setActiveExtension(const std::string& name, bool bypassPathLoss);
    void clearActiveExtension();
    bool isExtensionActive() const;
    const std::string& getActiveExtension() const;
    bool extensionBypassesPathLoss() const;

    void setExtensionPathLossDb(float pl_db);
    float getExtensionPathLossDb() const;

   private:
    void createPortCombinations();
    void setMeasuredSNRdB(float snr_db);
    void applyNoise(signal_v& signal, size_t size);
    LinkBudget computeLinkBudget(const NodeConfig& src_config,
                                 const NodeConfig& dest_config,
                                 double frequency_hz) const;
    float computePathLoss(float dist, const ChannelProcessParams& params) const;

   private:
    std::string _source;  // Signal source
    std::string _dest;    // Signal destination

    uint8_t _txNumCh;
    uint8_t _rxNumCh;

    // TODO should be vector of tuples
    //
    // TODO create struct or model for here
    std::map<chId, signal_v> _chTaps;
    std::map<chId, float> _portChCoeffs;

    NodeLink _linkInfo;
    float _txHeight{1.5f};
    float _rxHeight{1.5f};
    std::string _3gppScenario{"UMa"};
    std::string _hataEnvironment{"URBAN"};
    float _itmRefractivity{301.0f};
    float _itmGroundConductivity{0.005f};
    float _itmGroundPermittivity{15.0f};
    int _itmClimateZone{5};

    chem::NoiseType _noiseType;
    chem::PathLossType _plType;
    float _plDb{0.0f};
    float _snrMeasured;
    std::optional<float> _snrOverride;
    double _shadowingStd;

    uint8_t _powerCounter{0};
    float _cachedSigPower{-100.0f};

    double _freqOffsetHz{0.0};
    std::vector<double> _freqOffsetPhase;

    bool _dopplerEnabled{false};
    double _dopplerHz{0.0};

    dsp::channel::CIRApplyMethod _cirApplyMethod{
        dsp::channel::CIRApplyMethod::FIR_CAUSAL};

    std::string _activeExtension{};
    bool _extensionBypassesPathLoss{false};
    float _extensionPathLossDb{0.0f};
};
}  // namespace chem
