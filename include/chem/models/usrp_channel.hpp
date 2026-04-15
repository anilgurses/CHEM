#pragma once

#include <string.h>

#include <exception>

#include "../common.h"
#include "../nlohmann/json.hpp"

using json = nlohmann::json;

namespace chem {
namespace usrp {
class Channel {
   public:
    Channel() {};

    Channel(double txFreq, double rxFreq, double txGain, double rxGain,
            std::string txAnt, std::string rxAnt)
        : m_txTuneFreq(txFreq),
          m_rxTuneFreq(rxFreq),
          m_txGain(txGain),
          m_rxGain(rxGain),
          m_txAnt(txAnt),
          m_rxAnt(rxAnt) {};

    ~Channel() {};

    bool compare(const Channel& c) const {
        return (m_txTuneFreq == c.m_txTuneFreq) &&
               (m_rxTuneFreq == c.m_rxTuneFreq) && (m_txGain == c.m_txGain) &&
               (m_rxGain == c.m_rxGain) && (m_txAnt == c.m_txAnt) &&
               (m_rxAnt == c.m_rxAnt);
    }

    void setTxFreq(double freq) { m_txTuneFreq = freq; }
    double getTxFreq() const { return m_txTuneFreq; }

    void setRxFreq(double freq) { m_rxTuneFreq = freq; }
    double getRxFreq() const { return m_rxTuneFreq; }

    void setTxGain(double gain) { m_txGain = gain; }
    double getTxGain() const { return m_txGain; }

    void setRxGain(double gain) { m_rxGain = gain; }
    double getRxGain() const { return m_rxGain; }

    void setTxAntenna(std::string antenna) { m_txAnt = antenna; }
    std::string getTxAntenna() const { return m_txAnt; }

    void setRxAntenna(std::string antenna) { m_rxAnt = antenna; }
    std::string getRxAntenna() const { return m_rxAnt; }

   private:
    double m_txTuneFreq = 0.0, m_rxTuneFreq = 0.0;
    double m_txGain = 0.0, m_rxGain = 0.0;
    std::string m_txAnt = "", m_rxAnt = "";
};

static void from_json(const json& j, Channel& c) {
    try {
        if (j.contains("tx")) {
            c.setTxFreq(j.at("tx").at("tune_freq").get<double>());
            c.setTxGain(j.at("tx").at("gain").get<double>());
            c.setTxAntenna(j.at("tx").at("antenna").get<std::string>());
        }
    } catch (std::exception& e) {
        LOG_DEBUG("USRP::CHANNEL::TX",
                  fmt::format("Couldn't parse the channel info! Cause: {}",
                              e.what()));
    }

    try {
        if (j.contains("rx")) {
            c.setRxFreq(j.at("rx").at("tune_freq").get<double>());
            c.setRxGain(j.at("rx").at("gain").get<double>());
            c.setRxAntenna(j.at("rx").at("antenna").get<std::string>());
        }
    } catch (std::exception& e) {
        LOG_DEBUG("USRP::CHANNEL::RX",
                  fmt::format("Couldn't parse the channel info! Cause: {}",
                              e.what()));
    }
}

static void to_json(json& j, const Channel& c) {
    try {
        if (c.getTxFreq() != 0.0) {
            j["tx"] = json{{"tune_freq", c.getTxFreq()},
                           {"gain", c.getTxGain()},
                           {"antenna", c.getTxAntenna()}};
        }
        if (c.getRxFreq() != 0.0) {
            j["rx"] = json{{"tune_freq", c.getRxFreq()},
                           {"gain", c.getRxGain()},
                           {"antenna", c.getRxAntenna()}};
        }
    } catch (std::exception& e) {
    }
}

class SampleRate {
   public:
    SampleRate() : m_txRate(0.0), m_rxRate(0.0) {};

    SampleRate(double txRate, double rxRate)
        : m_txRate(txRate), m_rxRate(rxRate) {};

    ~SampleRate() {};

    friend bool operator==(const SampleRate& lhs, const SampleRate& rhs) {
        return lhs.m_txRate == rhs.m_txRate && lhs.m_rxRate == rhs.m_rxRate;
    }

    friend bool operator!=(const SampleRate& lhs, const SampleRate& rhs) {
        return !(lhs == rhs);
    }

    void setTxRate(double rate) { m_txRate = rate; }
    double getTxRate() const { return m_txRate; }

    void setRxRate(double rate) { m_rxRate = rate; }
    double getRxRate() const { return m_rxRate; }

   private:
    double m_txRate;
    double m_rxRate;
};

static void to_json(json& j, const SampleRate& s) {
    try {
        j = json{{"tx", s.getTxRate()}, {"rx", s.getRxRate()}};
    } catch (std::exception& e) {
        spdlog::warn(
            "Error occured while converting the USRP::SAMPLE RATE info to "
            "json. "
            "Cause : {}",
            e.what());
    }
}

static void from_json(const json& j, SampleRate& s) {
    try {
        if (j.contains("tx") && j.at("tx").is_number()) {
            s.setTxRate(j.at("tx").get<double>());
        } else {
            LOG_DEBUG("USRP::SAMPLE_RATE::TX",
                      "Missing or invalid 'tx' field in 'sample_rate' object");
            s.setTxRate(1000000.0);  // 1 Msps
        }
    } catch (std::exception& e) {
        LOG_DEBUG("USRP::SAMPLE_RATE::TX",
                  fmt::format("Couldn't parse the sample rate info! Cause: {}",
                              e.what()));
    }
    try {
        if (j.contains("rx") && j.at("rx").is_number()) {
            s.setRxRate(j.at("rx").get<double>());
        } else {
            LOG_DEBUG("USRP::SAMPLE_RATE::RX",
                      "Missing or invalid 'rx' field in 'sample_rate' object");
            s.setRxRate(1000000.0);  // 1 Msps
        }
    } catch (std::exception& e) {
        LOG_DEBUG("USRP::SAMPLE_RATE::RX",
                  fmt::format("Couldn't parse the sample rate info! Cause: {}",
                              e.what()));
    }
}
}  // namespace usrp
}  // namespace chem
