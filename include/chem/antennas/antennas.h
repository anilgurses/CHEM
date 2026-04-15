#pragma once

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "dipole.h"

#if __has_include("chem/antennas/generated/SA_1400_5900.h")
#include "chem/antennas/generated/SA_1400_5900.h"
#define CHEM_HAS_GEN_SA_1400_5900 1
#endif

#if __has_include("chem/antennas/generated/RM_WB1_DN_BLK_Right_Side_Up.h")
#include "chem/antennas/generated/RM_WB1_DN_BLK_Right_Side_Up.h"
#define CHEM_HAS_GEN_RM_WB1_DN_BLK_RIGHT_SIDE_UP 1
#endif

#if __has_include("chem/antennas/generated/RM_WB1_DN_BLK_Upside_Down.h")
#include "chem/antennas/generated/RM_WB1_DN_BLK_Upside_Down.h"
#define CHEM_HAS_GEN_RM_WB1_DN_BLK_UPSIDE_DOWN 1
#endif

namespace chem::antennas {

inline std::string normalize_name(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (auto& ch : name) {
        if (ch == ' ' || ch == '-') ch = '_';
    }
    // Collapse consecutive underscores
    std::string out;
    bool last_underscore = false;
    for (char ch : name) {
        if (ch == '_') {
            if (last_underscore) continue;
            last_underscore = true;
        } else {
            last_underscore = false;
        }
        out.push_back(ch);
    }
    if (!out.empty() && out.front() == '_') out.erase(out.begin());
    if (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? name : out;
}

inline std::vector<std::string> AvailableAntennas() {
    std::vector<std::string> names = {"dipole"};
#ifdef CHEM_HAS_GEN_SA_1400_5900
    names.push_back("sa-1400-5900");
#endif
#ifdef CHEM_HAS_GEN_RM_WB1_DN_BLK_RIGHT_SIDE_UP
    names.push_back("rm-wb1-dn-blk right side up");
#endif
#ifdef CHEM_HAS_GEN_RM_WB1_DN_BLK_UPSIDE_DOWN
    names.push_back("rm-wb1-dn-blk upside down");
#endif
    return names;
}

inline std::unique_ptr<Antenna> MakeAntenna(const std::string& name,
                                            double frequency_hz) {
    const std::string norm = normalize_name(name);

    if (norm == "isotropic") {
        return std::make_unique<IsotropicAntenna>();
    }
    if (norm == "dipole") {
        return std::make_unique<DipoleAntenna>(frequency_hz);
    }
#ifdef CHEM_HAS_GEN_SA_1400_5900
    if (norm == "sa_1400_5900") {
        return std::make_unique<chem::antennas::generated::SA_1400_5900>(
            frequency_hz);
    }
#endif
#ifdef CHEM_HAS_GEN_RM_WB1_DN_BLK_RIGHT_SIDE_UP
    if (norm == "rm_wb1_dn_blk_right_side_up") {
        return std::make_unique<
            chem::antennas::generated::RM_WB1_DN_BLK_Right_Side_Up>(
            frequency_hz);
    }
#endif
#ifdef CHEM_HAS_GEN_RM_WB1_DN_BLK_UPSIDE_DOWN
    if (norm == "rm_wb1_dn_blk_upside_down") {
        return std::make_unique<
            chem::antennas::generated::RM_WB1_DN_BLK_Upside_Down>(frequency_hz);
    }
#endif
    return nullptr;
}

}  // namespace chem::antennas
