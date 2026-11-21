#include "dht22_telemetry_adapter.hpp"

namespace dockstation::firmware::esp32 {
namespace {

constexpr std::int16_t kMinimumTemperatureTenthsC = -400;
constexpr std::int16_t kMaximumTemperatureTenthsC = 800;
constexpr std::uint16_t kMaximumHumidityTenthsPercent = 999U;

using dockstation::hardware::interfaces::ControllerTelemetryMeasurement;
using dockstation::hardware::interfaces::ControllerSensorValue;
using dockstation::hardware::interfaces::SensorQualityStatus;

}  // namespace

Dht22TelemetryBuildStatus make_dht22_telemetry_measurements(
    const DhtSensorReading& reading,
    std::string_view temperature_sensor_id,
    std::string_view humidity_sensor_id,
    std::string_view measured_at,
    SensorQualityStatus quality,
    std::array<ControllerTelemetryMeasurement, 2U>& output) noexcept {
    output = {};

    if (temperature_sensor_id.empty() ||
        humidity_sensor_id.empty() ||
        temperature_sensor_id == humidity_sensor_id ||
        measured_at.empty() ||
        !dockstation::hardware::interfaces::is_known_sensor_quality_status(quality)) {
        return Dht22TelemetryBuildStatus::InvalidArgument;
    }

    if (reading.temperature_tenths_c < kMinimumTemperatureTenthsC ||
        reading.temperature_tenths_c > kMaximumTemperatureTenthsC ||
        reading.relative_humidity_tenths_percent > kMaximumHumidityTenthsPercent) {
        return Dht22TelemetryBuildStatus::InvalidReading;
    }

    output[0] = ControllerTelemetryMeasurement{
        temperature_sensor_id,
        ControllerSensorValue{static_cast<double>(reading.temperature_tenths_c) / 10.0},
        "°C",
        quality,
        measured_at
    };
    output[1] = ControllerTelemetryMeasurement{
        humidity_sensor_id,
        ControllerSensorValue{
            static_cast<double>(reading.relative_humidity_tenths_percent) / 10.0},
        "%RH",
        quality,
        measured_at
    };

    return Dht22TelemetryBuildStatus::Success;
}

}  // namespace dockstation::firmware::esp32
