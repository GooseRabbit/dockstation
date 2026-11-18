#include "controller_station_state.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

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

const ControllerTelemetryMeasurement* find(
    const std::array<ControllerTelemetryMeasurement, 8>& measurements,
    std::string_view id) {
    for (const auto& measurement : measurements) {
        if (measurement.sensor_id == id) {
            return &measurement;
        }
    }
    return nullptr;
}
}  // namespace

int main() {
    ControllerStationState station;

    ControllerStationSafetyInputs safe{};
    safe.lid_position = LidPositionStatus::Closed;
    safe.workspace_known = true;
    safe.workspace_clear = true;
    station.set_safety_inputs(safe);

    const auto open = station.request_command(ControllerStationCommandType::OpenLid);
    expect(open.status == ControllerStationCommandDecisionStatus::Allow,
           "safe open request is accepted by local policy");

    auto snapshot = station.snapshot();
    expect(snapshot.safety.lid_position == LidPositionStatus::Closed &&
               snapshot.pending_command.has_value() &&
               *snapshot.pending_command == ControllerStationCommandType::OpenLid,
           "accepted command becomes pending without pretending that the lid opened");

    std::array<ControllerTelemetryMeasurement, 8> telemetry{};
    const auto count = station.build_telemetry(telemetry, "t1");
    expect(count == telemetry.size(), "station emits a fixed telemetry snapshot");

    const auto* lid = find(telemetry, "lid.position");
    const auto* pending = find(telemetry, "command.pending");
    expect(lid != nullptr &&
               std::get<std::string_view>(lid->value) == "CLOSED" &&
               lid->quality == SensorQualityStatus::Ok,
           "telemetry reports confirmed lid position, not requested position");
    expect(pending != nullptr &&
               std::get<std::string_view>(pending->value) == "PENDING",
           "telemetry exposes a pending command separately");

    safe.lid_position = LidPositionStatus::Unknown;
    station.set_safety_inputs(safe);
    const auto denied = station.request_command(ControllerStationCommandType::OpenLid);
    expect(denied.status == ControllerStationCommandDecisionStatus::DenyUnknownLidPosition,
           "unknown lid position denies a new normal command");

    snapshot = station.snapshot();
    expect(!snapshot.pending_command.has_value() &&
               snapshot.last_command_status ==
                   ControllerStationCommandDecisionStatus::DenyUnknownLidPosition,
           "denied command clears the pending request and records its reason");

    const auto stop = station.request_command(ControllerStationCommandType::StopMotion);
    expect(stop.status == ControllerStationCommandDecisionStatus::Allow,
           "safe stop remains available even with unknown lid position");

    snapshot = station.snapshot();
    expect(snapshot.pending_command.has_value() &&
               *snapshot.pending_command == ControllerStationCommandType::StopMotion,
           "stop request becomes pending without changing physical state");

    station.clear_pending_command();
    expect(!station.snapshot().pending_command.has_value(),
           "pending request can be cleared after actuator acknowledgement");

    safe.safety_stop_active = true;
    station.set_safety_inputs(safe);
    const auto blocked = station.request_command(ControllerStationCommandType::StartCleaning);
    expect(blocked.status == ControllerStationCommandDecisionStatus::DenySafetyStop,
           "safety stop blocks normal operation");

    telemetry.fill({});
    station.build_telemetry(telemetry, "t2");
    const auto* command_result = find(telemetry, "command.last_decision");
    expect(command_result != nullptr &&
               std::get<std::string_view>(command_result->value) ==
                   "DENY_SAFETY_STOP" &&
               command_result->quality == SensorQualityStatus::Ok,
           "telemetry reports the last command decision");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
