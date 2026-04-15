/**
 * @file node.hpp
 * @brief
 * @author Anıl Gürses
 * @version v1.1.0
 * @date 2023-06-17
 */
#pragma once
#include <string.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace db {

struct NodeSpecs {
    std::string id;
    double txFreq;
    double rxFreq;
    int numChannels;
    double txsampleRate;
    double rxsampleRate;
    double chCoeff;
    std::string vehicleAddress;
    std::string antennaPattern;
};

void to_json(json& j, const NodeSpecs& n) {
    j["id"] = n.id;
    j["txFreq"] = n.txFreq;
    j["rxFreq"] = n.rxFreq;
    j["numChannels"] = n.numChannels;
    j["txsampleRate"] = n.txsampleRate;
    j["rxsampleRate"] = n.rxsampleRate;
    j["chCoeff"] = n.chCoeff;
    j["vehicleAddress"] = n.vehicleAddress;
    j["antennaPattern"] = n.antennaPattern;
}

void from_json(const json& j, NodeSpecs& n) {
    j.at("id").get_to(n.id);
    j.at("txFreq").get_to(n.txFreq);
    j.at("rxFreq").get_to(n.rxFreq);
    j.at("numChannels").get_to(n.numChannels);
    j.at("txsampleRate").get_to(n.txsampleRate);
    j.at("rxsampleRate").get_to(n.rxsampleRate);
    j.at("chCoeff").get_to(n.chCoeff);
    if (j.contains("vehicleAddress"))
        j.at("vehicleAddress").get_to(n.vehicleAddress);
    j.at("antennaPattern").get_to(n.antennaPattern);
}

struct Node {
    std::string name;
    NodeSpecs specs;
};

struct ChSpecs {
    std::string pathLoss;
    double sRate;
    double shadowingSTD;
};

void to_json(json& j, const ChSpecs& c) {
    j["pathLoss"] = c.pathLoss;
    j["sRate"] = c.sRate;
}

void from_json(const json& j, ChSpecs& c) {
    j.at("pathLoss").get_to(c.pathLoss);
    j.at("sRate").get_to(c.sRate);
}

struct Channel {
    std::string freq;
    ChSpecs specs;
};

}  // namespace db
