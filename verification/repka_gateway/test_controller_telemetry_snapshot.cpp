#include "hardware/interfaces/controller_telemetry_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace dockstation::hardware::interfaces;

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

ControllerCodecDecodedMessage make_message(
    const ControllerTelemetryMeasurement* measurements,
    std::size_t count,
    std::uint64_t sequence) {
    ControllerCodecDecodedMessage message{};
    message.envelope.type = ControllerMessageType::TelemetrySnapshot;
    message.envelope.protocol_version = ControllerProtocolVersion{1U, 1U};
    message.envelope.controller_id = "esp32-01";
    message.envelope.controller_instance_id = "boot-0001";
    message.envelope.sequence_number = sequence;
    message.envelope.sent_at = "1794800000000";
    message.payload = ControllerTelemetrySnapshotPayload{measurements, count};
    return message;
}
}  // namespace

int main() {
    std::array<char, 16U> sensor_id{};
    std::array<char, 8U> unit{};
    std::array<char, 16U> measured_at{};
    constexpr char sensor[] = "dht22-01";
    constexpr char units[] = "degC";
    constexpr char when[] = "m-001";
    std::copy(sensor, sensor + sizeof(sensor) - 1U, sensor_id.begin());
    std::copy(units, units + sizeof(units) - 1U, unit.begin());
    std::copy(when, when + sizeof(when) - 1U, measured_at.begin());

    const ControllerTelemetryMeasurement measurement{
        std::string_view{sensor_id.data(), sizeof(sensor) - 1U},
        23.75,
        std::string_view{unit.data(), sizeof(units) - 1U},
        SensorQualityStatus::Ok,
        std::string_view{measured_at.data(), sizeof(when) - 1U}};
    const auto message = make_message(&measurement, 1U, 42U);

    ControllerTelemetrySnapshot snapshot{};
    expect(!snapshot.available(),
           "snapshot starts unavailable rather than inventing measurements");
    expect(snapshot.replace(message, 1794800000123ULL),
           "valid accepted telemetry snapshot is copied");
    expect(snapshot.available() && snapshot.measurement_count() == 1U &&
               snapshot.sequence_number() == 42U &&
               snapshot.received_at_unix_ms() == 1794800000123ULL,
           "snapshot metadata is retained");

    sensor_id.fill('x');
    unit.fill('x');
    measured_at.fill('x');
    ControllerTelemetryMeasurementView view{};
    expect(snapshot.measurement(0U, view) &&
               view.sensor_id == "dht22-01" &&
               view.unit == "degC" &&
               view.measured_at == "m-001" &&
               std::get<double>(view.value) == 23.75 &&
               view.quality == SensorQualityStatus::Ok,
           "copied text and typed value survive source-buffer reuse");
    expect(snapshot.controller_id() == "esp32-01" &&
               snapshot.controller_instance_id() == "boot-0001",
           "controller identity is copied into owned storage");
    expect(!snapshot.measurement(1U, view),
           "out-of-range measurement lookup is rejected");

    const std::array<ControllerTelemetryMeasurement,
        kControllerCodecMaximumMeasurements + 1U> too_many{};
    const auto invalid_count = make_message(
        too_many.data(), too_many.size(), 43U);
    expect(!snapshot.replace(invalid_count, 1794800000999ULL),
           "snapshot exceeding fixed codec capacity is rejected");
    expect(snapshot.sequence_number() == 42U &&
               snapshot.measurement_count() == 1U,
           "failed replacement preserves the previous accepted snapshot");

    auto wrong_type = message;
    wrong_type.envelope.type = ControllerMessageType::Heartbeat;
    expect(!snapshot.replace(wrong_type, 1794800000999ULL),
           "non-telemetry message cannot replace the snapshot");

    auto zero_sequence = message;
    zero_sequence.envelope.sequence_number = 0U;
    expect(!snapshot.replace(zero_sequence, 1794800000999ULL),
           "zero sequence is rejected");

    const auto empty_message = make_message(nullptr, 0U, 44U);
    expect(snapshot.replace(empty_message, 1794800001000ULL) &&
               snapshot.available() && snapshot.measurement_count() == 0U &&
               snapshot.sequence_number() == 44U,
           "explicit empty snapshot is retained without inventing sensor values");

    snapshot.clear();
    expect(!snapshot.available() && snapshot.measurement_count() == 0U &&
               snapshot.controller_id().empty(),
           "clear removes retained telemetry");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
