#pragma once

#include "dht22_frame_decoder.hpp"
#include "hardware/interfaces/controller_protocol.hpp"

#include <array>
#include <string_view>

namespace dockstation::firmware::esp32 {

// Maps one decoded DHT22 reading to temperature and humidity measurements.
// IDs, timestamp and quality belong to the caller; this helper does not
// acquire data, generate time or infer freshness.
enum class Dht22TelemetryBuildStatus : std::uint8_t {
    Success,
    InvalidArgument,
    InvalidReading
};

// On failure, output is cleared. Returned string views refer to caller-owned
// IDs/timestamp and must remain valid while the measurements are in use.
[[nodiscard]] Dht22TelemetryBuildStatus make_dht22_telemetry_measurements(
    const Dht22DecodedReading& reading,
    std::string_view temperature_sensor_id,
    std::string_view humidity_sensor_id,
    std::string_view measured_at,
    dockstation::hardware::interfaces::SensorQualityStatus quality,
    std::array<dockstation::hardware::interfaces::ControllerTelemetryMeasurement, 2U>& output) noexcept;

}  // namespace dockstation::firmware::esp32
