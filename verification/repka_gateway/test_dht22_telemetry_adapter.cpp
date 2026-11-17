#include "dht22_telemetry_adapter.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <variant>

using namespace dockstation::firmware::esp32;
using dockstation::hardware::interfaces::ControllerTelemetryMeasurement;
using dockstation::hardware::interfaces::SensorQualityStatus;

namespace {
int checks = 0;
int failures = 0;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        std::cerr << "FAIL - " << description << '\n';
        ++failures;
    }
}

bool output_is_clear(
    const std::array<ControllerTelemetryMeasurement, 2U>& output) {
    for (const auto& measurement : output) {
        if (!measurement.sensor_id.empty() ||
            !measurement.unit.empty() ||
            !measurement.measured_at.empty() ||
            measurement.quality != SensorQualityStatus::Unknown ||
            !std::holds_alternative<bool>(measurement.value) ||
            std::get<bool>(measurement.value)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    std::array<ControllerTelemetryMeasurement, 2U> output{};
    const Dht22DecodedReading positive{269, 658U};

    const auto success = make_dht22_telemetry_measurements(
        positive, "climate.outdoor.temperature", "climate.outdoor.humidity",
        "2026-11-17T04:00:00Z", SensorQualityStatus::Ok, output);

    expect(success == Dht22TelemetryBuildStatus::Success,
           "valid DHT22 reading creates two measurements");
    expect(output[0].sensor_id == "climate.outdoor.temperature" &&
               output[1].sensor_id == "climate.outdoor.humidity",
           "caller-supplied sensor IDs are preserved");
    expect(output[0].unit == "°C" && output[1].unit == "%RH",
           "temperature and relative humidity use explicit units");
    expect(std::holds_alternative<double>(output[0].value) &&
               std::holds_alternative<double>(output[1].value) &&
               std::abs(std::get<double>(output[0].value) - 26.9) < 0.0001 &&
               std::abs(std::get<double>(output[1].value) - 65.8) < 0.0001,
           "integer tenths are converted to physical numeric values");
    expect(output[0].measured_at == "2026-11-17T04:00:00Z" &&
               output[1].measured_at == "2026-11-17T04:00:00Z",
           "caller-supplied measurement time is preserved");
    expect(output[0].quality == SensorQualityStatus::Ok &&
               output[1].quality == SensorQualityStatus::Ok,
           "caller-supplied quality is preserved");

    const Dht22DecodedReading negative{-101, 658U};
    const auto stale = make_dht22_telemetry_measurements(
        negative, "sensor.temperature", "sensor.humidity", "source-time",
        SensorQualityStatus::Stale, output);
    expect(stale == Dht22TelemetryBuildStatus::Success &&
               std::holds_alternative<double>(output[0].value) &&
               std::abs(std::get<double>(output[0].value) - (-10.1)) < 0.0001 &&
               output[0].quality == SensorQualityStatus::Stale,
           "negative temperature and caller-selected stale quality are preserved");

    expect(make_dht22_telemetry_measurements(
               positive, "", "sensor.humidity", "time",
               SensorQualityStatus::Ok, output) ==
               Dht22TelemetryBuildStatus::InvalidArgument &&
               output_is_clear(output),
           "missing sensor ID is rejected and output is cleared");

    expect(make_dht22_telemetry_measurements(
               positive, "same", "same", "time",
               SensorQualityStatus::Ok, output) ==
               Dht22TelemetryBuildStatus::InvalidArgument &&
               output_is_clear(output),
           "duplicate sensor IDs are rejected and output is cleared");

    expect(make_dht22_telemetry_measurements(
               positive, "sensor.temperature", "sensor.humidity", "",
               SensorQualityStatus::Ok, output) ==
               Dht22TelemetryBuildStatus::InvalidArgument &&
               output_is_clear(output),
           "missing measurement time is rejected and output is cleared");

    expect(make_dht22_telemetry_measurements(
               positive, "sensor.temperature", "sensor.humidity", "time",
               static_cast<SensorQualityStatus>(255U), output) ==
               Dht22TelemetryBuildStatus::InvalidArgument &&
               output_is_clear(output),
           "unknown quality enum is rejected and output is cleared");

    const Dht22DecodedReading invalid_temperature{801, 500U};
    expect(make_dht22_telemetry_measurements(
               invalid_temperature, "sensor.temperature", "sensor.humidity",
               "time", SensorQualityStatus::Ok, output) ==
               Dht22TelemetryBuildStatus::InvalidReading &&
               output_is_clear(output),
           "out-of-range decoded data is rejected and output is cleared");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
