#pragma once

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <deque>
#include <filesystem>
#include <vector>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <volk/volk.h>

#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"

// Network constants
#define UDP

struct Header {
    size_t size;  // Doesn't include header size
    int64_t start;
    int64_t end;
    uint64_t rt_tx_time;
    uint64_t sample_rate;
    uint32_t number_of_samples;
    uint16_t total_frags;
    uint8_t number_of_channels;
};
// For better memory alignment
static_assert(sizeof(Header) == 48, "Header size must be 48 bytes");

struct UDPHeader {
    uint64_t timestamp;
    size_t size;  // Size of the UDP payload
    uint16_t seq_number;
    // For whether it is start, end
    // Flag 0x1 for start, 0x2 for end, 0x4 for data
    uint8_t flag;
    // Padding for memory alignment
    std::uint8_t _pad[5];
};
// For better memory alignment
static_assert(sizeof(UDPHeader) == 24, "UDP Header size must be 24 bytes");

#define HEADER_SIZE sizeof(struct Header)
#define UDP_HEADER_SIZE sizeof(struct UDPHeader)
#define MTU_SIZE 1472  // MTU (1500B) - IPHDR (20B) - UDPHDR (8B) = 1472B
#define BATCH_SIZE 4
#define UDP_CAP 16  // UDP payload pool capacity
#define PAYLOAD_SIZE (MTU_SIZE - UDP_HEADER_SIZE)
#define MAX_FRAGS 128  // Limiting fragmentation
#define MAX_IQ (PAYLOAD_SIZE * MAX_FRAGS)

#define OTW_SIZE 4
#define PAYLOAD_N_SAMPLES (PAYLOAD_SIZE / OTW_SIZE)
// Color codes
#define RESET "\033[0m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define BLUE "\033[34m"
#define WHITE "\033[37m"

// Math constants
#define PI 3.14159265
#define SPEED_OF_LIGHT 299792458
#define BOLTZMANN 1.38064852e-23
#define PLANCK 6.62607015e-34
#define EARTH_RADIUS 6371000

// Channel constants
// 20*log10(4pi/c) FSPL
#define FSPL_CONSTANT -147.55
#define LOG_SAFE_GUARD 0.0001
// Guard for values used as arguments to log()/log10(); clamps non-finite or
// non-positive values.
static inline float LOG_GUARD(float x) {
    return std::max<float>(static_cast<float>(LOG_SAFE_GUARD),
                           std::isfinite(x) ? x : 0.0f);
}
// dBm in 1 Hz bandwidth (This is what we've observer in experiments)
// TODO: Change it in future when we have more information about the noise
// floor on RF frontends
#define NOISE_FIGURE_RX 6      // dB (USRP datasheet says 5-8 dB)
#define NOISE_POWER -174       // dBm in 1 Hz bandwidth
#define NOISE_POWER_DBFS -174  // dBm in 1 Hz bandwidth

static inline float NOISE_FLOOR(float bw) {
    return static_cast<float>(NOISE_POWER) + 10.0f * std::log10(bw);
}

static inline float NOISE_FLOOR_DBFS(float bw) {
    return static_cast<float>(NOISE_POWER_DBFS) + 10.0f * std::log10(bw) +
           NOISE_FIGURE_RX;
}

#define TX_GAIN 15  // dBm | Default value if none provided
#define RX_GAIN 10  // dBm | Default value if none provided
// TODO: Need to add cable loss etc.
#define NOISE_SIZE 20000
#define SHADOWING_STD \
    0  // Was 3; set to 0 to test if per-buffer shadowing causes SNR compression
#define SHADOWING_MEAN 0

// Time conversion
static inline float NS_2_MS(float x) { return x / 1.0e6f; }
static inline float NS_2_US(float x) { return x / 1.0e3f; }
static inline float NS_2_S(float x) { return x / 1.0e9f; }
static inline float MS_2_NS(float x) { return x * 1.0e6f; }
static inline float S_2_NS(float x) { return x * 1.0e9f; }

// dB conversions
static inline float DB_TO_LIN_A(float x) { return std::pow(10.0f, x / 20.0f); }
static inline float DBM_TO_LIN(float x) {
    return std::pow(10.0f, (x - 30.0f) / 10.0f);
}
static inline float DBM_TO_LIN_A(float x) {
    return std::pow(10.0f, (x - 30.0f) / 20.0f);
}
static inline float LIN_TO_DBM(float x) {
    return 10.0f * std::log10(x) + 30.0f;
}
static inline float DB_TO_LIN(float x) { return std::pow(10.0f, x / 10.0f); }
static inline float LIN_TO_DB(float x) { return 10.0f * std::log10(x); }
static inline float LIN_TO_DB_A(float x) { return 20.0f * std::log10(x); }
static inline float DBM_TO_DB(float x) { return x - 30.0f; }
static inline float DB_TO_DBM(float x) { return x + 30.0f; }

// Frequency conversion
#define HZ_TO_KHZ(x) ((x) / 1e3)
#define HZ_TO_MHZ(x) ((x) / 1e6)
#define HZ_TO_GHZ(x) ((x) / 1e9)
#define KHZ_TO_HZ(x) ((x) * 1e3)
#define MHZ_TO_HZ(x) ((x) * 1e6)
#define GHZ_TO_HZ(x) ((x) * 1e9)

// Port number for receiver
// Port number shoukd start from 1
#define UL_PORT_NUM(x) ((x) + 10000)
#define REP_PORT_NUM(x) ((x) + 11000)
#define REQ_PORT_NUM(x) ((x) + 10000)
#define COORD_PORT_NUM 5000
#define RECV_TIMEOUT 1000
#define SEND_TIMEOUT 1000
#define TIMEOUT_LIMIT 3
#define DEF_DB_PATH "/tmp/chem.db3"

// TODO: Change it based on the configuration
// Pool constants
#define POOL_CAP 32
#define QUEUE_CAP 500
#define IQ_POOL_CAP (QUEUE_CAP * 4)
#define IQ_SIZE (MAX_IQ * 4)  // 4x oversampling

// Some constants for operation
#define BYTE_ALIGNMENT 32
#define CH_UPD_RATE 100  // ms
#define TIME_RATIO 1000000000

const std::string ACHEM_OPENING_MSG = R"(
      _____ __ __ ____ __  ___
     / ___// // // __//  |/  /
    / /__ / _  // _/ / /|_/ /
    \___//_//_//___//_/  /_/  @ 2022 AERPAW)";

inline std::string _getFolderName(const std::string_view path) {
    std::filesystem::path p(path);
    return p.parent_path().filename().string();
}

namespace chem {

namespace log {

template <typename MsgT>
inline void emit(spdlog::level::level_enum level, std::string_view caller,
                 const MsgT& msg) {
    if (auto logger = spdlog::get("CHEM")) {
        logger->log(level, "[\033[34m{:^14}\033[0m] {}", caller, msg);
    }
}

class LogStream {
   public:
    LogStream(spdlog::level::level_enum level, std::string_view caller)
        : level_(level), caller_(caller) {}

    template <typename T>
    LogStream& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

    LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        stream_ << manip;
        return *this;
    }

    LogStream& operator<<(std::ios_base& (*manip)(std::ios_base&)) {
        stream_ << manip;
        return *this;
    }

    ~LogStream() noexcept {
        try {
            emit(level_, caller_, stream_.str());
        } catch (...) {
        }
    }

   private:
    spdlog::level::level_enum level_;
    std::string caller_;
    std::ostringstream stream_;
};

}  // namespace log

using fc = std::complex<float>;

template <typename T>
struct volk_allocator {
    using value_type = T;
    volk_allocator() noexcept = default;
    template <typename U>
    volk_allocator(const volk_allocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        return static_cast<T*>(
            volk_malloc(n * sizeof(T), volk_get_alignment()));
    }
    void deallocate(T* p, std::size_t) noexcept { volk_free(p); }
    template <typename U>
    bool operator==(const volk_allocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const volk_allocator<U>&) const noexcept { return false; }
};

using signal_v = std::vector<fc, volk_allocator<fc>>;

using signal_queue = std::deque<fc>;

enum class NodeType {
    UNKNOWN = 0,
    FIXED,
    AERPAW_FIXED,
    AERPAW_PORTABLE,
    VEHICLE_MAVPROXY
};

inline constexpr const char* NodeTypeToString(NodeType t) {
    switch (t) {
        case NodeType::FIXED:
            return "Fixed";
        case NodeType::AERPAW_FIXED:
            return "AERPAW Fixed";
        case NodeType::AERPAW_PORTABLE:
            return "AERPAW Portable";
        case NodeType::VEHICLE_MAVPROXY:
            return "Vehicle (MAVProxy)";
        default:
            return "Unknown";
    }
}

enum class CommType { TX, RX };

enum class NoiseType { NONE, AWGN };

enum class PropagationModel {
    UNKNOWN = 0,
    FREE_SPACE,
    TWO_RAY,
    THREE_GPP_38_901,
    OKUMURA_HATA,
    LONGLEY_RICE,
    NONE
};

inline std::string PropagationModelToString(PropagationModel model) {
    switch (model) {
        case PropagationModel::FREE_SPACE:
            return "FREE_SPACE";
        case PropagationModel::TWO_RAY:
            return "2_RAY";
        case PropagationModel::THREE_GPP_38_901:
            return "3GPP_38_901";
        case PropagationModel::OKUMURA_HATA:
            return "OKUMURA_HATA";
        case PropagationModel::LONGLEY_RICE:
            return "LONGLEY_RICE";
        case PropagationModel::NONE:
            return "NONE";
        case PropagationModel::UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

inline PropagationModel PropagationModelFromString(std::string_view model) {
    std::string upper(model);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    if (upper == "FREE_SPACE") return PropagationModel::FREE_SPACE;
    if (upper == "2_RAY" || upper == "TWO_RAY")
        return PropagationModel::TWO_RAY;
    if (upper == "3GPP_38_901" || upper == "3GPP")
        return PropagationModel::THREE_GPP_38_901;
    if (upper == "OKUMURA_HATA" || upper == "HATA")
        return PropagationModel::OKUMURA_HATA;
    if (upper == "LONGLEY_RICE" || upper == "ITM")
        return PropagationModel::LONGLEY_RICE;
    if (upper == "NONE") return PropagationModel::NONE;
    return PropagationModel::UNKNOWN;
}

struct PropagationDefaults {
    std::atomic<PropagationModel> model{PropagationModel::FREE_SPACE};
    std::atomic<float> groundCoeff{-1.0f};
    std::string scenario{"UMa"};
    std::mutex scenarioMutex;
    std::string hataEnvironment{"URBAN"};
    std::atomic<float> refractivity{301.0f};
    std::atomic<float> groundConductivity{0.005f};
    std::atomic<float> groundPermittivity{15.0f};
    std::atomic<int> climateZone{5};
};

enum class PathLossType {
    twoRay,
    freeSpace,
    multiPath,
    threeGPP_38_901,
    okumuraHata,
    longleyRice
};

struct NodeLink {
    NodeType type{NodeType::UNKNOWN};
    float dist = 0;
    float h_dist = 0;
    float elevation = 0;  // DoA
    float azimuth = 0;    // AoD
};

struct Location {
    double lat{0.0};
    double lon{0.0};
    float alt{0.0f};
};

// Local tangent-plane velocity in meters/second (ENU).
struct Velocity {
    float east{0.0f};
    float north{0.0f};
    float up{0.0f};
};

struct NodeLocation {
    uint8_t logicalNode{0};
    Location position{};
    Velocity velocity{};
    bool hasVelocity{false};
};

inline std::optional<Location> ResolveAerpawLocation(
    const std::string& key_raw) {
    std::string upper;
    upper.resize(key_raw.size());
    std::transform(
        key_raw.begin(), key_raw.end(), upper.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (upper == "CC-1" || upper == "CC1")
        return Location{35.773871026537634, -78.67694781603241, 10.0f};
    if (upper == "CC-2" || upper == "CC2")
        return Location{35.772182310053005, -78.67476986245003, 10.0f};
    if (upper == "CC-3" || upper == "CC3")
        return std::nullopt;  // Unknown; will be updated later
    if (upper == "LW-1" || upper == "LW1")
        return Location{35.72750947, -78.69595819, 12.0f};
    if (upper == "LW-2" || upper == "LW2")
        return Location{35.72821305, -78.70090823, 10.0f};
    if (upper == "LW-3" || upper == "LW3")
        return Location{35.72491205, -78.69190014, 20.0f};
    if (upper == "LW-4" || upper == "LW4")
        return Location{35.73318358, -78.6983642, 12.0f};
    if (upper == "LW-5" || upper == "LW5")
        return Location{35.74294142, -78.69962993, 18.0f};
    // PN (portable node) is mobile; location will arrive via telemetry
    return std::nullopt;
}

}  // namespace chem

#define _CHEM_LOG_DISPATCH(_1, _2, NAME, ...) NAME

#define _CHEM_LOG_INFO_1(caller) \
    ::chem::log::LogStream(spdlog::level::info, caller)
#define _CHEM_LOG_INFO_2(caller, msg) \
    ::chem::log::emit(spdlog::level::info, caller, msg)

#define _CHEM_LOG_DEBUG_1(caller) \
    ::chem::log::LogStream(spdlog::level::debug, caller)
#define _CHEM_LOG_DEBUG_2(caller, msg) \
    ::chem::log::emit(spdlog::level::debug, caller, msg)

#define _CHEM_LOG_CRITICAL_1(caller) \
    ::chem::log::LogStream(spdlog::level::critical, caller)
#define _CHEM_LOG_CRITICAL_2(caller, msg) \
    ::chem::log::emit(spdlog::level::critical, caller, msg)

#define _CHEM_LOG_ERROR_1(caller) \
    ::chem::log::LogStream(spdlog::level::err, caller)
#define _CHEM_LOG_ERROR_2(caller, msg) \
    ::chem::log::emit(spdlog::level::err, caller, msg)

#define _CHEM_LOG_WARN_1(caller) \
    ::chem::log::LogStream(spdlog::level::warn, caller)
#define _CHEM_LOG_WARN_2(caller, msg) \
    ::chem::log::emit(spdlog::level::warn, caller, msg)

#define LOG_INFO(...) \
    _CHEM_LOG_DISPATCH(__VA_ARGS__, _CHEM_LOG_INFO_2, _CHEM_LOG_INFO_1) \
    (__VA_ARGS__)
#define LOG_DEBUG(...) \
    _CHEM_LOG_DISPATCH(__VA_ARGS__, _CHEM_LOG_DEBUG_2, _CHEM_LOG_DEBUG_1) \
    (__VA_ARGS__)
#define LOG_CRITICAL(...) \
    _CHEM_LOG_DISPATCH(__VA_ARGS__, _CHEM_LOG_CRITICAL_2, \
                       _CHEM_LOG_CRITICAL_1) \
    (__VA_ARGS__)
#define LOG_ERROR(...) \
    _CHEM_LOG_DISPATCH(__VA_ARGS__, _CHEM_LOG_ERROR_2, _CHEM_LOG_ERROR_1) \
    (__VA_ARGS__)
#define LOG_WARN(...) \
    _CHEM_LOG_DISPATCH(__VA_ARGS__, _CHEM_LOG_WARN_2, _CHEM_LOG_WARN_1) \
    (__VA_ARGS__)
