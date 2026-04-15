#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../aerpaw/node_characteristics.h"
#include "../common.h"
#include "../nlohmann/json.hpp"
#include "usrp_channel.hpp"

using json = nlohmann::json;

namespace chem {

class NodeConfig {
   public:
    void setId(std::string id) { m_id = id; }
    std::string getId() const { return m_id; }

    void setName(std::string name) { m_name = name; }
    std::string getName() const { return m_name; }

    void setIpAddress(const std::string& IpAddress) { m_ipAddress = IpAddress; }
    std::string getIpAddress() const { return m_ipAddress; }

    void setTxPortNumber(const short& port) { m_portTxNum = port; }

    short getTxPortNumber() const { return m_portTxNum; }

    void setRxPortNumber(const short& port) { m_portRxNum = port; }

    short getRxPortNumber() const { return m_portRxNum; }

    void updateTxFreq(const double& freq, const uint8_t& ch) {
        m_channels.at(ch).setTxFreq(freq);
    }

    void updateRxFreq(const double& freq, const uint8_t& ch) {
        m_channels.at(ch).setRxFreq(freq);
    }

    void setSampleRate(usrp::SampleRate sampleRate) {
        m_sample_rate = sampleRate;
    }

    usrp::SampleRate getSampleRate() const { return m_sample_rate; }

    void setChannels(std::vector<usrp::Channel> chList) { m_channels = chList; }

    void addChannel(usrp::Channel ch) { m_channels.emplace_back(ch); }

    std::vector<usrp::Channel> getChannels() const { return m_channels; }

    void setNumChannels(uint8_t numChannels) { m_numChannels = numChannels; }
    uint8_t getNumChannels() const { return m_numChannels; }

    void setLogicalNode(int logicalNode) { m_logicalNode = logicalNode; }
    int getLogicalNode() const { return m_logicalNode; }
    bool hasLogicalNode() const { return m_logicalNode >= 0; }

    void setLocation(const chem::Location& location) {
        m_location = location;
        m_hasLocation = true;
    }
    chem::Location getLocation() const { return m_location; }
    bool hasLocation() const { return m_hasLocation; }

    void setVehicleAddress(std::string address) {
        m_vehicleAddress = std::move(address);
    }
    const std::string& getVehicleAddress() const { return m_vehicleAddress; }
    bool hasVehicleAddress() const { return !m_vehicleAddress.empty(); }
    void setNodeType(chem::NodeType nodeType) { m_nodeType = nodeType; }
    chem::NodeType getNodeType() const { return m_nodeType; }
    void setAerpawKey(std::string key) { m_aerpawKey = std::move(key); }
    const std::string& getAerpawKey() const { return m_aerpawKey; }
    bool hasAerpawKey() const { return !m_aerpawKey.empty(); }
    void setCharacteristics(
        const chem::aerpaw::NodeCharacteristics& characteristics) {
        m_characteristics = characteristics;
    }
    void clearCharacteristics() { m_characteristics.reset(); }
    bool hasCharacteristics() const { return m_characteristics.has_value(); }
    const std::optional<chem::aerpaw::NodeCharacteristics>& getCharacteristics()
        const {
        return m_characteristics;
    }

    void setDeviceGains(double txGain, double rxGain) {
        setDeviceTxGain(txGain);
        setDeviceRxGain(rxGain);
    }
    void setDeviceTxGain(double txGain) {
        m_deviceTxGain = txGain;
        m_hasDeviceTxGain = true;
    }
    void setDeviceRxGain(double rxGain) {
        m_deviceRxGain = rxGain;
        m_hasDeviceRxGain = true;
    }
    double getDeviceTxGain() const { return m_deviceTxGain; }
    double getDeviceRxGain() const { return m_deviceRxGain; }
    bool hasDeviceTxGain() const { return m_hasDeviceTxGain; }
    bool hasDeviceRxGain() const { return m_hasDeviceRxGain; }

    void setTxAntennaPattern(std::string pattern) {
        m_txAntennaPattern = std::move(pattern);
        m_hasTxAntennaPattern = true;
    }
    void setRxAntennaPattern(std::string pattern) {
        m_rxAntennaPattern = std::move(pattern);
        m_hasRxAntennaPattern = true;
    }
    void setAntennaPattern(std::string pattern) {
        setTxAntennaPattern(pattern);
        setRxAntennaPattern(std::move(pattern));
    }
    const std::string& getTxAntennaPattern() const {
        return m_txAntennaPattern;
    }
    const std::string& getRxAntennaPattern() const {
        return m_rxAntennaPattern;
    }
    bool hasTxAntennaPattern() const { return m_hasTxAntennaPattern; }
    bool hasRxAntennaPattern() const { return m_hasRxAntennaPattern; }

    void setInputFormat(std::string format) {
        m_inputFormat = std::move(format);
    }
    const std::string& getInputFormat() const { return m_inputFormat; }

    void setOutputFormat(std::string format) {
        m_outputFormat = std::move(format);
    }
    const std::string& getOutputFormat() const { return m_outputFormat; }

    friend void to_json(json& j, const NodeConfig& n);
    friend void from_json(const json& j, NodeConfig& n);

   private:
    std::string m_ipAddress;
    std::string m_id = "";
    std::string m_name;
    short m_portTxNum, m_portRxNum;
    uint8_t m_numChannels = 1;
    usrp::SampleRate m_sample_rate;
    std::vector<usrp::Channel> m_channels{};
    int m_logicalNode = -1;
    chem::Location m_location{};
    bool m_hasLocation = false;
    std::string m_vehicleAddress;
    std::string m_aerpawKey;
    chem::NodeType m_nodeType = chem::NodeType::UNKNOWN;
    std::optional<chem::aerpaw::NodeCharacteristics> m_characteristics;
    double m_deviceTxGain = 0.0;
    double m_deviceRxGain = 0.0;
    bool m_hasDeviceTxGain = false;
    bool m_hasDeviceRxGain = false;
    std::string m_txAntennaPattern;
    std::string m_rxAntennaPattern;
    bool m_hasTxAntennaPattern = false;
    bool m_hasRxAntennaPattern = false;
    std::string m_inputFormat;
    std::string m_outputFormat;
};

inline void to_json(json& j, const NodeConfig& n) {
    try {
        j = json{{"id", n.m_id},
                 {"name", n.m_name},
                 {"numChannels", n.m_numChannels},
                 {"sample_rate", n.m_sample_rate}};
        if (!n.m_channels.empty()) {
            j["channels"] = n.m_channels;
        }
        if (n.m_logicalNode >= 0) {
            j["logical_node"] = n.m_logicalNode;
        }
        if (n.m_hasLocation) {
            j["location"] = json{{"lat", n.m_location.lat},
                                 {"lon", n.m_location.lon},
                                 {"alt", n.m_location.alt}};
        }
        if (!n.m_vehicleAddress.empty()) {
            j["vehicle"] = json{{"address", n.m_vehicleAddress}};
        }
        if (!n.m_aerpawKey.empty()) {
            j["AERPAW"] = n.m_aerpawKey;
        }
        if (n.m_nodeType != chem::NodeType::UNKNOWN) {
            j["node_type"] = static_cast<int>(n.m_nodeType);
        }
        if (n.m_hasDeviceTxGain || n.m_hasDeviceRxGain) {
            json gains;
            if (n.m_hasDeviceTxGain) gains["tx"] = n.m_deviceTxGain;
            if (n.m_hasDeviceRxGain) gains["rx"] = n.m_deviceRxGain;
            if (!gains.empty()) {
                j["gains"] = gains;
            }
        }
        if (!n.m_inputFormat.empty()) {
            j["inputFormat"] = n.m_inputFormat;
        }
        if (!n.m_outputFormat.empty()) {
            j["outputFormat"] = n.m_outputFormat;
        }
    } catch (std::exception& e) {
        LOG_DEBUG("NODE_CONFIG", "Couldn't convert to json!");
    }
}

inline void from_json(const json& j, NodeConfig& n) {
    try {
        if (j.contains("id")) n.m_id = j.at("id").get<std::string>();
        if (j.contains("name")) n.m_name = j.at("name").get<std::string>();
    } catch (std::exception& e) {
        LOG_DEBUG("NODE_CONFIG",
                  fmt::format("Couldn't parse id/name! Cause: {}", e.what()));
    }

    try {
        if (j.contains("numChannels") && j.at("numChannels").is_number()) {
            n.m_numChannels = j.at("numChannels").get<uint8_t>();
        }
    } catch (std::exception& e) {
        LOG_DEBUG(
            "NODE_CONFIG",
            fmt::format("Couldn't parse numChannels! Cause: {}", e.what()));
    }

    try {
        if (j.contains("channels")) {
            n.m_channels = j.at("channels").get<std::vector<usrp::Channel>>();
        } else if (n.m_numChannels > 0) {
            n.m_channels = std::vector<usrp::Channel>(n.m_numChannels);
        }
    } catch (std::exception& e) {
        LOG_DEBUG("NODE_CONFIG",
                  fmt::format("Couldn't parse channels! Cause: {}", e.what()));
    }

    try {
        if (j.contains("sample_rate")) {
            n.m_sample_rate = j.at("sample_rate").get<usrp::SampleRate>();
        }
    } catch (std::exception& e) {
        LOG_DEBUG(
            "NODE_CONFIG",
            fmt::format("Couldn't parse sample_rate! Cause: {}", e.what()));
    }

    try {
        if (j.contains("logical_node")) {
            if (j.at("logical_node").is_number()) {
                n.m_logicalNode = j.at("logical_node").get<int>();
            } else if (j.at("logical_node").is_string()) {
                n.m_logicalNode =
                    std::stoi(j.at("logical_node").get<std::string>());
            }
        }
    } catch (std::exception& e) {
        LOG_DEBUG(
            "NODE_CONFIG",
            fmt::format("Couldn't parse logical_node! Cause: {}", e.what()));
    }

    try {
        if (j.contains("location") && j.at("location").is_object()) {
            auto loc = j.at("location");
            chem::Location location{};
            if (loc.contains("lat")) {
                if (loc.at("lat").is_number()) {
                    location.lat = loc.at("lat").get<double>();
                } else {
                    location.lat = std::stod(loc.at("lat").get<std::string>());
                }
            }
            if (loc.contains("lon")) {
                if (loc.at("lon").is_number()) {
                    location.lon = loc.at("lon").get<double>();
                } else {
                    location.lon = std::stod(loc.at("lon").get<std::string>());
                }
            }
            if (loc.contains("alt")) {
                if (loc.at("alt").is_number()) {
                    location.alt = loc.at("alt").get<float>();
                } else {
                    location.alt = std::stof(loc.at("alt").get<std::string>());
                }
            }
            n.setLocation(location);
        }
    } catch (std::exception& e) {
        LOG_DEBUG("NODE_CONFIG",
                  fmt::format("Couldn't parse location! Cause: {}", e.what()));
    }

    try {
        if (j.contains("vehicle") && j.at("vehicle").is_object()) {
            auto v = j.at("vehicle");
            if (v.contains("address") && v.at("address").is_string()) {
                n.m_vehicleAddress = v.at("address").get<std::string>();
            }
        }
    } catch (std::exception& e) {
        LOG_DEBUG("NODE_CONFIG",
                  fmt::format("Couldn't parse vehicle! Cause: {}", e.what()));
    }

    try {
        auto parseAerpawObj = [&](const json& aerpawObj) {
            if (aerpawObj.contains("node") &&
                aerpawObj.at("node").is_string()) {
                n.m_aerpawKey = aerpawObj.at("node").get<std::string>();
            }
        };

        if (j.contains("AERPAW")) {
            if (j.at("AERPAW").is_string()) {
                n.m_aerpawKey = j.at("AERPAW").get<std::string>();
            } else if (j.at("AERPAW").is_object()) {
                parseAerpawObj(j.at("AERPAW"));
            }
        } else if (j.contains("aerpaw")) {
            if (j.at("aerpaw").is_string()) {
                n.m_aerpawKey = j.at("aerpaw").get<std::string>();
            } else if (j.at("aerpaw").is_object()) {
                parseAerpawObj(j.at("aerpaw"));
            }
        }
    } catch (std::exception& e) {
        LOG_DEBUG(
            "NODE_CONFIG",
            fmt::format("Couldn't parse AERPAW key! Cause: {}", e.what()));
    }

    try {
        if (j.contains("gains") && j.at("gains").is_object()) {
            auto g = j.at("gains");
            if (g.contains("tx") && g.at("tx").is_number()) {
                n.m_deviceTxGain = g.at("tx").get<double>();
                n.m_hasDeviceTxGain = true;
            }
            if (g.contains("rx") && g.at("rx").is_number()) {
                n.m_deviceRxGain = g.at("rx").get<double>();
                n.m_hasDeviceRxGain = true;
            }
        }

        if (!n.m_channels.empty()) {
            if (!n.m_hasDeviceTxGain) {
                const double tx = n.m_channels.front().getTxGain();
                if (std::isfinite(tx) && std::abs(tx) > 1e-12) {
                    n.m_deviceTxGain = tx;
                    n.m_hasDeviceTxGain = true;
                }
            }
            if (!n.m_hasDeviceRxGain) {
                const double rx = n.m_channels.front().getRxGain();
                if (std::isfinite(rx) && std::abs(rx) > 1e-12) {
                    n.m_deviceRxGain = rx;
                    n.m_hasDeviceRxGain = true;
                }
            }
        }
    } catch (std::exception& e) {
        LOG_DEBUG("NODE_CONFIG",
                  fmt::format("Couldn't parse gains! Cause: {}", e.what()));
    }

    try {
        if (j.contains("node_type") && j.at("node_type").is_number_integer()) {
            n.m_nodeType =
                static_cast<chem::NodeType>(j.at("node_type").get<int>());
        }
    } catch (std::exception& e) {
        LOG_DEBUG("NODE_CONFIG",
                  fmt::format("Couldn't parse node_type! Cause: {}", e.what()));
    }

    try {
        if (j.contains("inputFormat") && j.at("inputFormat").is_string()) {
            n.m_inputFormat = j.at("inputFormat").get<std::string>();
        }
        if (j.contains("outputFormat") && j.at("outputFormat").is_string()) {
            n.m_outputFormat = j.at("outputFormat").get<std::string>();
        }
    } catch (std::exception& e) {
        LOG_DEBUG("NODE_CONFIG",
                  fmt::format("Couldn't parse input/output format! Cause: {}",
                              e.what()));
    }
}

}  // namespace chem
