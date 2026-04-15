#include "chem/channel/channel.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "chem/dsp/antenna_prop.h"
#include "chem/dsp/channel.h"
#include "chem/dsp/signal.h"
#include "chem/dsp/utils.h"
#include "chem/models/node_config.hpp"
#include "spdlog/fmt/fmt.h"

using namespace chem;

Channel::Channel(std::string source, std::string dest, uint8_t txNumCh,
                 uint8_t rxNumCh)
    : _source(std::move(source)),
      _dest(std::move(dest)),
      _txNumCh(txNumCh),
      _rxNumCh(rxNumCh),
      _linkInfo(),
      _noiseType(NoiseType::AWGN),
      _plType(PathLossType::freeSpace),
      _snrMeasured(0.0f),
      _snrOverride(std::nullopt),
      _shadowingStd(SHADOWING_STD),
      _freqOffsetHz(0.0),
      _freqOffsetPhase(static_cast<size_t>(txNumCh), 0.0),
      _dopplerEnabled(false),
      _dopplerHz(0.0) {
    createPortCombinations();
}

void Channel::createPortCombinations() {
    for (size_t c0 = 0; c0 < _txNumCh; c0++) {
        for (size_t c1 = 0; c1 < _rxNumCh; c1++) {
            chId t_ch;
            t_ch.src = c0;
            t_ch.dest = c1;
            _portChCoeffs.insert(std::make_pair(t_ch, 1.));
        }
    }
}

std::string Channel::getSrc() const { return _source; }

std::string Channel::getDest() const { return _dest; }

// ---------------------------------------------------------------------------
// Link geometry
// ---------------------------------------------------------------------------

void Channel::updateDistance(const float& distance) {
    _linkInfo.dist = distance;
}

float Channel::getLinkDistance() const { return _linkInfo.dist; }

void Channel::updateAltitude(const float& alt) { _linkInfo.h_dist = alt; }

void Channel::updateHeights(const float& tx_height, const float& rx_height) {
    if (std::isfinite(tx_height)) _txHeight = tx_height;
    if (std::isfinite(rx_height)) _rxHeight = rx_height;
}

void Channel::updateElevation(const float& elevation) {
    _linkInfo.elevation = elevation;
}

void Channel::updateAzimuth(const float& azimuth) {
    _linkInfo.azimuth = azimuth;
}

// ---------------------------------------------------------------------------
// Model configuration
// ---------------------------------------------------------------------------

void Channel::updateNoiseType(chem::NoiseType noise) { _noiseType = noise; }

chem::NoiseType Channel::getNoiseType() const { return _noiseType; }

void Channel::updatePlType(chem::PathLossType pl) { _plType = pl; }

chem::PathLossType Channel::getPlType() const { return _plType; }

void Channel::setPl(float pl_db) { _plDb = pl_db; }

float Channel::getPlDb() const { return _plDb; }

void Channel::update3gppScenario(std::string scenario) {
    if (!scenario.empty()) _3gppScenario = std::move(scenario);
}

void Channel::updateHataEnvironment(std::string environment) {
    if (!environment.empty()) _hataEnvironment = std::move(environment);
}

void Channel::updateITMParams(float refractivity, float ground_conductivity,
                              float ground_permittivity, int climate_zone) {
    if (std::isfinite(refractivity)) _itmRefractivity = refractivity;
    if (std::isfinite(ground_conductivity))
        _itmGroundConductivity = ground_conductivity;
    if (std::isfinite(ground_permittivity))
        _itmGroundPermittivity = ground_permittivity;
    _itmClimateZone = std::clamp(climate_zone, 1, 7);
}

void Channel::setShadowingSTD(double std) { _shadowingStd = std; }

void Channel::setActiveExtension(const std::string& name, bool bypassPathLoss) {
    _activeExtension = name;
    _extensionBypassesPathLoss = bypassPathLoss;
}

void Channel::clearActiveExtension() {
    _activeExtension.clear();
    _extensionBypassesPathLoss = false;
    _extensionPathLossDb = 0.0f;
}

bool Channel::isExtensionActive() const { return !_activeExtension.empty(); }

const std::string& Channel::getActiveExtension() const {
    return _activeExtension;
}

bool Channel::extensionBypassesPathLoss() const {
    return _extensionBypassesPathLoss;
}

void Channel::setExtensionPathLossDb(float pl_db) {
    _extensionPathLossDb = pl_db;
}

float Channel::getExtensionPathLossDb() const {
    return _extensionPathLossDb;
}

void Channel::updateSNR(const float& snr) { _snrOverride = snr; }

void Channel::clearSNROverride() { _snrOverride.reset(); }

bool Channel::hasSNROverride() const { return _snrOverride.has_value(); }

float Channel::getSNR() const { return _snrOverride.value_or(_snrMeasured); }

float Channel::getMeasuredSNRdB() const { return _snrMeasured; }

void Channel::setMeasuredSNRdB(float snr_db) { _snrMeasured = snr_db; }

// ---------------------------------------------------------------------------
// Port coefficients
// ---------------------------------------------------------------------------

// RF Port Num
float Channel::getChCoeffs(const uint8_t& src, const uint8_t& dest) {
    chId t_ch;
    t_ch.src = src;
    t_ch.dest = dest;

    auto _port = _portChCoeffs.find(t_ch);
    if (_port != _portChCoeffs.end()) {
        return _port->second;
    }
    return 0.;
}

void Channel::updateChCoeff(const chId& ch, const float& val) {
    auto _port = _portChCoeffs.find(ch);
    if (_port != _portChCoeffs.end()) _port->second = val;
}

std::map<chId, float> Channel::getIndChannels() const { return _portChCoeffs; }

// ---------------------------------------------------------------------------
// CIR (channel impulse response)
// ---------------------------------------------------------------------------

void Channel::setCIRApplyMethod(dsp::channel::CIRApplyMethod method) {
    _cirApplyMethod = method;
}

dsp::channel::CIRApplyMethod Channel::getCIRApplyMethod() const {
    return _cirApplyMethod;
}

void Channel::updateChTaps(const chId& ch,
                           const signal_v& taps) {
    _chTaps[ch] = taps;
}

void Channel::clearChTaps(const chId& ch) { _chTaps.erase(ch); }

size_t Channel::getChTapCount(const chId& ch) const {
    auto it = _chTaps.find(ch);
    if (it == _chTaps.end()) return 0;
    return it->second.size();
}

// ---------------------------------------------------------------------------
// Frequency offset & Doppler
// ---------------------------------------------------------------------------

void Channel::setFrequencyOffsetHz(double offset_hz) {
    if (!std::isfinite(offset_hz)) {
        offset_hz = 0.0;
    }
    _freqOffsetHz = offset_hz;
}

double Channel::getFrequencyOffsetHz() const { return _freqOffsetHz; }

void Channel::setDopplerEnabled(bool enabled) { _dopplerEnabled = enabled; }

bool Channel::isDopplerEnabled() const { return _dopplerEnabled; }

void Channel::setDopplerHz(double doppler_hz) {
    if (!std::isfinite(doppler_hz)) doppler_hz = 0.0;
    _dopplerHz = doppler_hz;
}

double Channel::getDopplerHz() const { return _dopplerHz; }

void Channel::applyFrequencyOffset(signal_v& signal, size_t size,
                                   double sample_rate_hz,
                                   size_t dest_channel_index) {
    const double total_offset_hz =
        _freqOffsetHz + (_dopplerEnabled ? _dopplerHz : 0.0);
    if (total_offset_hz == 0.0) return;
    if (dest_channel_index >= _freqOffsetPhase.size()) return;
    dsp::signal::apply_frequency_offset(signal, size, sample_rate_hz,
                                        total_offset_hz,
                                        _freqOffsetPhase[dest_channel_index]);
}

// ---------------------------------------------------------------------------
// Core processing
// ---------------------------------------------------------------------------

LinkBudget Channel::computeLinkBudget(const NodeConfig& src_config,
                                      const NodeConfig& dest_config,
                                      double frequency_hz) const {
    LinkBudget budget{};
    budget.tx_gain_db = static_cast<float>(
        src_config.hasDeviceTxGain() ? src_config.getDeviceTxGain() : TX_GAIN);
    budget.rx_gain_db = static_cast<float>(dest_config.hasDeviceRxGain()
                                               ? dest_config.getDeviceRxGain()
                                               : RX_GAIN);
    budget.noise_figure_db = 0;

    if (src_config.hasCharacteristics()) {
        const auto& chars = src_config.getCharacteristics();
        if (chars.has_value()) {
            budget.tx_gain_db += static_cast<float>(chars->tx_pa_gain_db);
            budget.noise_figure_db = chars->tx_noise_figure_db;
            budget.tx_cable_loss_db = static_cast<float>(chars->cable_loss_db);
            budget.source_power_dbfs = chars->source_power_dbfs;
        }
    }

    if (dest_config.hasCharacteristics()) {
        const auto& chars = dest_config.getCharacteristics();
        if (chars.has_value()) {
            budget.rx_gain_db += static_cast<float>(chars->rx_gain_db);
            budget.noise_figure_db += chars->rx_noise_figure_db;
            budget.rx_cable_loss_db = static_cast<float>(chars->cable_loss_db);
        }
    }

    budget.tx_ant_gain_db = chem::dsp::antenna::compute_antenna_gain_db(
        src_config, true, frequency_hz, _linkInfo.elevation, _linkInfo.azimuth);
    budget.rx_ant_gain_db = chem::dsp::antenna::compute_antenna_gain_db(
        dest_config, false, frequency_hz, _linkInfo.elevation,
        _linkInfo.azimuth);

    return budget;
}

float Channel::computePathLoss(float dist,
                               const ChannelProcessParams& params) const {
    // When an extension bypasses statistical models, use the extension-provided
    // path loss value instead. This is 0 dB if the extension encodes propagation
    // entirely in CIR taps, or a computed value if the extension provides its own
    // path loss calculation.
    if (_extensionBypassesPathLoss) return _extensionPathLossDb;

    switch (params.propagationModel) {
        case PropagationModel::FREE_SPACE:
            return dsp::channel::calc_fspl(
                dist, static_cast<float>(params.freq), _shadowingStd);
        case PropagationModel::TWO_RAY:
            return dsp::channel::calc_2ray(
                dist, static_cast<float>(params.freq), _txHeight, _rxHeight,
                params.ground_coeff, _shadowingStd);
        case PropagationModel::THREE_GPP_38_901: {
            const float ue_height = std::max<float>(0.01f, _rxHeight);
            const float bs_height = std::max<float>(0.01f, _txHeight);
            return dsp::channel::calc_3gpp_38_901(
                dist, static_cast<float>(params.freq), ue_height, bs_height,
                _3gppScenario, _shadowingStd);
        }
        case PropagationModel::OKUMURA_HATA: {
            const float bs_height = std::max<float>(1.0f, _txHeight);
            const float ms_height = std::max<float>(1.0f, _rxHeight);
            return dsp::channel::calc_okumura_hata(
                dist, static_cast<float>(params.freq), bs_height, ms_height,
                _hataEnvironment, _shadowingStd);
        }
        case PropagationModel::LONGLEY_RICE:
            return dsp::channel::calc_longley_rice(
                dist, static_cast<float>(params.freq), _txHeight, _rxHeight,
                _itmRefractivity, _itmGroundConductivity,
                _itmGroundPermittivity, _itmClimateZone, _shadowingStd);
        case PropagationModel::NONE:
        case PropagationModel::UNKNOWN:
        default:
            return 0.0f;
    }
}

void Channel::applyNoise(signal_v& signal, size_t size) {
    if (_noiseType == NoiseType::AWGN) {
        dsp::channel::apply_awgn(signal, signal);
    }
}

void Channel::processChannel(signal_v& src,
                             signal_v& dest,
                             const ChannelProcessParams& params) {
    using namespace std::chrono;

    if (!params.src_config || !params.dest_config) return;

    if (params.timing) {
        params.timing->t_start =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count();
    }

    const auto budget =
        computeLinkBudget(*params.src_config, *params.dest_config, params.freq);

    const float gain_scalar = std::isfinite(params.coeff) ? params.coeff : 0.0f;
    const float dist = getLinkDistance();

    if (params.sample_ratio != 1) {
        dsp::signal::resample(src, dest, params.sample_ratio,
                              params.s_perBuff / params.sample_ratio);
        dsp::utils::fc32_mul_scalar(dest, gain_scalar, dest, params.s_perBuff);
    } else {
        dsp::utils::fc32_mul_scalar(src, gain_scalar, dest, params.s_perBuff);
    }

    const bool src_sc16 = (params.src_config->getOutputFormat() == "sc16") ||
                          (params.src_config->getInputFormat() == "sc16");

    if (params.timing) {
        params.timing->t_resample =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count();
    }

    // Calculate power every 4th call to reduce overhead
    float sig_power;
    constexpr float kMinSigPower = -100.0f;
    const bool use_ref_power = std::isfinite(budget.source_power_dbfs);

    if (use_ref_power) {
        sig_power = budget.source_power_dbfs;
    } else {
        // Compute power on the first call (_powerCounter == 0) and every 4th
        // call after
        ++_powerCounter;
        if (_powerCounter >= 4 || _cachedSigPower <= kMinSigPower) {
            _powerCounter = 0;
            sig_power = LIN_TO_DB(
                chem::dsp::signal::calc_power(dest, params.s_perBuff) +
                LOG_SAFE_GUARD);
            _cachedSigPower = sig_power;
        } else {
            sig_power = _cachedSigPower;
        }
    }

    const bool has_signal =
        std::isfinite(sig_power) && sig_power > kMinSigPower;
    const float sig_power_safe = has_signal ? sig_power : kMinSigPower;
    const float sig_ref = use_ref_power
                              ? static_cast<float>(budget.source_power_dbfs)
                              : sig_power_safe;

    const double noise_bw =
        (std::isfinite(params.sample_rate_hz) && params.sample_rate_hz > 0.0)
            ? params.sample_rate_hz
            : 1.0;
    float pl_db = 1.0f;
    float snr_db = 0.0f;
    float p_tx = sig_ref + budget.tx_gain_db + budget.tx_ant_gain_db -
                 budget.tx_cable_loss_db;  // in dBm
    float p_rx = 0.0f;

    const chId cir_key{params.dest_channel_index, params.src_channel_index};
    const auto taps_it = _chTaps.find(cir_key);
    if (_cirApplyMethod != dsp::channel::CIRApplyMethod::NONE &&
        taps_it != _chTaps.end() && !taps_it->second.empty()) {
        if (_cirApplyMethod == dsp::channel::CIRApplyMethod::DIRECT_CONV) {
            // grow to actual signal size  
            thread_local signal_v cir_out;
            if (cir_out.size() < params.s_perBuff) {
                cir_out.resize(params.s_perBuff, fc(0.0f, 0.0f));
            }
            dsp::channel::apply_cir(dest, cir_out, taps_it->second,
                                    params.s_perBuff, _cirApplyMethod);
            std::copy_n(cir_out.begin(), params.s_perBuff, dest.begin());
        } else {
            dsp::channel::apply_cir(dest, dest, taps_it->second,
                                    params.s_perBuff, _cirApplyMethod);
        }
    }

    if (params.timing) {
        params.timing->t_cir =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count();
    }

    pl_db = computePathLoss(dist, params);

    if (params.timing) {
        params.timing->t_pathloss =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count();
    }

    setPl(pl_db);
    p_rx = p_tx - pl_db + budget.rx_gain_db + budget.rx_ant_gain_db -
           budget.rx_cable_loss_db;  // in dBm
    // Do not allow received power to be greater than 0 dBFS
    // - Otherwise it will saturate
    p_rx = std::min(p_rx, 0.0f);
    double noise_figure_db = budget.noise_figure_db;
    noise_figure_db +=
        budget.rx_gain_db;  // RX gain amplifies receiver noise floor
    const auto& noise = dsp::channel::get_noise(params.s_perBuff, noise_bw,
                                                _noiseType, noise_figure_db);
    float meas_noise_power =
        LIN_TO_DB(chem::dsp::signal::calc_power(noise, params.s_perBuff));
    float rx_scale_db = p_rx - sig_ref;
    const float rx_scale = std::sqrt(DB_TO_LIN(rx_scale_db));
    snr_db = sig_ref + rx_scale_db - meas_noise_power;  // in dB

    dsp::utils::fc32_mul_scalar_sum(dest, rx_scale, noise, dest,
                                    params.s_perBuff);

    if (src_sc16) {
        // This is due to the way OAI handles scaling for sc16, 12 bit DAC
        // output. The signal is effectively 4 bits lower than a full scale
        // fc32, so we need to shift it back up to get the correct power levels
        // for path loss and noise calculations.
        dsp::utils::fc32_lshift(dest, 4, dest, params.s_perBuff);
    }

    if (params.timing) {
        params.timing->t_noise =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count();
    }

    applyFrequencyOffset(dest, params.s_perBuff, params.sample_rate_hz,
                         params.dest_channel_index);

    if (params.timing) {
        params.timing->t_freq_offset =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count();
    }

    if (has_signal || use_ref_power) {
        setMeasuredSNRdB(snr_db);
    }

    if (params.timing) {
        params.timing->t_end =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
                .count();
    }
}
