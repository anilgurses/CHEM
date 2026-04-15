#include "chem/vehicle/handler.h"

#include <mavsdk/log_callback.h>
#include <spdlog/fmt/fmt.h>

#include <iostream>

#include "chem/runtime_config.h"

using namespace chem;
using namespace chem::vehicle;

using std::chrono::seconds;

Handler::Handler(std::string connection, std::string nodeId, chem::Node& node)
    : connection_str(connection), m_NodeId(nodeId), m_node(&node) {
    mavsdk::log::subscribe(
        [](mavsdk::log::Level level,    // message severity level
           const std::string& message,  // message text
           const std::string&
               file,    // source file from which the message was sent
           int line) {  // line number in the source file
            // returning true from the callback disables printing the message to
            // stdout
            return level < mavsdk::log::Level::Err;
        });
}

Handler::~Handler() { LOG_WARN("HANDLER", "Vehicle is disconnected"); }

void Handler::Start() {
    auto setFallbackLocation = [this]() {
        if (!m_node) return;

        auto fallback = chem::ResolveAerpawLocation("LW-1");
        if (!fallback.has_value()) return;

        uint8_t logicalNode = 0;
        if (m_node->getConfig().hasLogicalNode() &&
            m_node->getConfig().getLogicalNode() >= 0) {
            logicalNode =
                static_cast<uint8_t>(m_node->getConfig().getLogicalNode());
        }
        m_node->setLocation(*fallback, logicalNode);
    };

    if (!Connect()) {
        LOG_ERROR("HANDLER",
                  fmt::format("Vehicle connection failed for node \"{}\" ({})",
                              m_NodeId, connection_str));
        setFallbackLocation();
        return;
    }

    size_t cnt = 0;

    while (mavsdk.systems().size() == 0 && cnt < 6) {
        std::this_thread::sleep_for(seconds(1));
        cnt++;
    }

    if (mavsdk.systems().size() == 0) {
        LOG_ERROR("HANDLER", fmt::format("Vehicle connection failed for node "
                                         "\"{}\"; no MAVSDK systems found",
                                         m_NodeId));
        setFallbackLocation();
        return;
    }

    for (auto system : mavsdk.systems()) {
        std::cout << "Found system with MAVLink system ID: "
                  << static_cast<int>(system->get_system_id())
                  << ", connected: " << (system->is_connected() ? "yes" : "no")
                  << ", has autopilot: "
                  << (system->has_autopilot() ? "yes" : "no") << '\n';
    }

    if (cnt >= 5) return;

    if (mavsdk.systems().size() == 0) {
        return;
    }

    std::shared_ptr<System> system = mavsdk.systems()[0];

    auto telemetry = Telemetry{system};

    auto to_rate_hz = [](int poll_ms) -> double {
        const int ms = std::max(20, poll_ms);
        return 1000.0 / static_cast<double>(ms);
    };

    double last_rate_hz =
        to_rate_hz(chem::RuntimeConfig::vehicle_poll_rate_ms.load());
    (void)telemetry.set_rate_position(last_rate_hz);

    _running = true;

    // TODO: Optimize here
    telemetry.subscribe_position([this](Telemetry::Position position) {
        if (!_running) return;
        relative_alt = position.relative_altitude_m;
        lat = position.latitude_deg;
        lon = position.longitude_deg;
        if (m_node) {
            m_node->setLocation(chem::Location{lat, lon, relative_alt}, 10);
        }
    });

    telemetry.subscribe_velocity_ned([this](Telemetry::VelocityNed vel) {
        if (!_running) return;
        if (!m_node) return;
        chem::Velocity v;
        v.north = vel.north_m_s;
        v.east = vel.east_m_s;
        v.up = -vel.down_m_s;
        m_node->setVelocity(v);
    });

    while (_running) {
        const double desired =
            to_rate_hz(chem::RuntimeConfig::vehicle_poll_rate_ms.load());
        if (std::abs(desired - last_rate_hz) > 1e-6) {
            (void)telemetry.set_rate_position(desired);
            last_rate_hz = desired;
        }
        std::this_thread::sleep_for(seconds(1));
    }
}

const double Handler::getLat() const { return lat; }

const double Handler::getLon() const { return lon; }

const float Handler::getAlt() const { return relative_alt; }

bool Handler::Connect() {
    ConnectionResult connection_result =
        mavsdk.add_any_connection(connection_str);

    if (connection_result != ConnectionResult::Success) {
        spdlog::info("Couldn't connect to Vehicle!");
        return false;
    }

    spdlog::info("Connected to Vehicle! {}", m_NodeId);

    return true;
}

bool Handler::isRunning() { return _running.load(); }
