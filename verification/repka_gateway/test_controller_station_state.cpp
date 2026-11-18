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
        if (measurement.sensor_id == id) return &measurement;
    }
    return nullptr;
}

ControllerStationInputFreshnessConfig freshness() {
    return {50U, 100U, 100U, 100U};
}

ControllerStationSafetyInputs safe_closed() {
    ControllerStationSafetyInputs safe{};
    safe.lid_position = LidPositionStatus::Closed;
    safe.platform_motion_active = false;
    safe.cleaning_active = false;
    safe.drive_motion_active = false;
    safe.safety_stop_active = false;
    safe.workspace_known = true;
    safe.workspace_clear = true;
    return safe;
}

void apply_inputs(
    ControllerStationState& station,
    const ControllerStationSafetyInputs& inputs,
    std::uint64_t monotonic_time_ms) {
    station.set_safety_inputs(inputs, monotonic_time_ms);
    station.update_lid_position(inputs.lid_position, monotonic_time_ms);
}
}  // namespace

int main() {
    ControllerStationState station;
    station.configure_input_freshness(freshness());

    auto safe = safe_closed();
    apply_inputs(station, safe, 1000U);

    const auto open = station.request_command(
        ControllerStationCommandType::OpenLid, 1050U);
    expect(open.status == ControllerStationCommandDecisionStatus::Allow,
           "fresh inputs remain valid at the exact max-age boundary");

    auto snapshot = station.snapshot();
    expect(snapshot.safety.lid_position == LidPositionStatus::Closed &&
               snapshot.pending_command.has_value() &&
               *snapshot.pending_command == ControllerStationCommandType::OpenLid,
           "accepted command becomes pending without pretending that the lid opened");

    std::array<ControllerTelemetryMeasurement, 8> telemetry{};
    expect(station.build_telemetry(telemetry, "t1", 1050U) == telemetry.size(),
           "station emits a fixed telemetry snapshot");
    const auto* lid = find(telemetry, "lid.position");
    const auto* pending = find(telemetry, "command.pending");
    expect(lid != nullptr &&
               std::get<std::string_view>(lid->value) == "CLOSED" &&
               lid->quality == SensorQualityStatus::Ok,
           "fresh telemetry reports confirmed lid position");
    expect(pending != nullptr &&
               std::get<std::string_view>(pending->value) == "PENDING",
           "telemetry exposes a pending command separately");

    const auto blocked_by_pending = station.request_command(
        ControllerStationCommandType::CloseLid, 1050U);
    snapshot = station.snapshot();
    expect(blocked_by_pending.status ==
               ControllerStationCommandDecisionStatus::DenyCommandPending &&
               snapshot.pending_command.has_value() &&
               *snapshot.pending_command == ControllerStationCommandType::OpenLid,
           "a second normal command cannot replace an accepted pending command");

    station.clear_pending_command();
    const auto stale_lid = station.request_command(
        ControllerStationCommandType::OpenLid, 1051U);
    expect(stale_lid.reason_code == "LID_POSITION_STALE",
           "lid feedback expires immediately after its configured freshness window");

    telemetry.fill({});
    static_cast<void>(station.build_telemetry(telemetry, "t2", 1051U));
    lid = find(telemetry, "lid.position");
    const auto* motion = find(telemetry, "platform.motion_active");
    const auto* stop_state = find(telemetry, "safety_stop.active");
    const auto* workspace = find(telemetry, "workspace.state");
    expect(lid != nullptr &&
               std::get<std::string_view>(lid->value) == "STALE" &&
               lid->quality == SensorQualityStatus::Stale,
           "stale lid telemetry is explicitly STALE");
    expect(motion != nullptr &&
               std::get<std::string_view>(motion->value) == "INACTIVE" &&
               motion->quality == SensorQualityStatus::Ok,
           "motion feedback remains fresh within its longer window");
    expect(stop_state != nullptr &&
               std::get<std::string_view>(stop_state->value) == "INACTIVE" &&
               stop_state->quality == SensorQualityStatus::Ok,
           "safety-stop feedback remains fresh within its configured window");
    expect(workspace != nullptr &&
               std::get<std::string_view>(workspace->value) == "CLEAR" &&
               workspace->quality == SensorQualityStatus::Ok,
           "workspace feedback remains fresh within its configured window");

    safe = safe_closed();
    apply_inputs(station, safe, 1100U);
    const auto recovered = station.request_command(
        ControllerStationCommandType::OpenLid, 1100U);
    expect(recovered.status == ControllerStationCommandDecisionStatus::Allow,
           "a new valid sample restores command eligibility");

    station.clear_pending_command();
    const auto stale_safety_stop = station.request_command(
        ControllerStationCommandType::OpenLid, 1201U);
    expect(stale_safety_stop.reason_code == "SAFETY_STOP_STALE",
           "stale safety-stop feedback blocks normal commands");

    telemetry.fill({});
    static_cast<void>(station.build_telemetry(telemetry, "t3", 1201U));
    const auto* stale_stop = find(telemetry, "safety_stop.active");
    expect(stale_stop != nullptr &&
               std::get<std::string_view>(stale_stop->value) == "STALE" &&
               stale_stop->quality == SensorQualityStatus::Stale,
           "stale safety-stop telemetry cannot masquerade as inactive");

    station.clear_pending_command();
    const auto stop = station.request_command(
        ControllerStationCommandType::StopMotion, 1201U);
    expect(stop.status == ControllerStationCommandDecisionStatus::Allow,
           "stop remains available even when safety inputs are stale");

    station.clear_pending_command();
    safe = safe_closed();
    safe.safety_stop_active = true;
    apply_inputs(station, safe, 1300U);
    const auto blocked = station.request_command(
        ControllerStationCommandType::StartCleaning, 1300U);
    expect(blocked.status == ControllerStationCommandDecisionStatus::DenySafetyStop,
           "a fresh active safety-stop blocks normal operation");

    telemetry.fill({});
    static_cast<void>(station.build_telemetry(telemetry, "t4", 1300U));
    const auto* command_result = find(telemetry, "command.last_decision");
    expect(command_result != nullptr &&
               std::get<std::string_view>(command_result->value) ==
                   "DENY_SAFETY_STOP" &&
               command_result->quality == SensorQualityStatus::Ok,
           "telemetry reports the last command decision");

    ControllerStationState independent_lid_age;
    independent_lid_age.configure_input_freshness(freshness());
    apply_inputs(independent_lid_age, safe_closed(), 1500U);
    // Updating the other safety inputs with a cached lid field must not refresh
    // the independently sampled end-stop position.
    independent_lid_age.set_safety_inputs(safe_closed(), 1540U);
    const auto cached_lid = independent_lid_age.request_command(
        ControllerStationCommandType::OpenLid, 1551U);
    expect(cached_lid.reason_code == "LID_POSITION_STALE",
           "updating motion/safety inputs cannot refresh a cached lid position");

    ControllerStationState motion_stale;
    motion_stale.configure_input_freshness({100U, 50U, 100U, 100U});
    apply_inputs(motion_stale, safe_closed(), 2000U);
    const auto stale_motion = motion_stale.request_command(
        ControllerStationCommandType::OpenLid, 2051U);
    expect(stale_motion.reason_code == "MOTION_FEEDBACK_STALE",
           "stale mechanism feedback blocks commands independently of lid freshness");

    ControllerStationState workspace_stale;
    workspace_stale.configure_input_freshness({100U, 100U, 100U, 50U});
    apply_inputs(workspace_stale, safe_closed(), 3000U);
    const auto stale_workspace = workspace_stale.request_command(
        ControllerStationCommandType::StartCleaning, 3051U);
    expect(stale_workspace.reason_code == "WORKSPACE_STATE_STALE",
           "stale workspace confirmation blocks cleaning independently");

    ControllerStationState clock_regression;
    clock_regression.configure_input_freshness(freshness());
    apply_inputs(clock_regression, safe_closed(), 4000U);
    const auto clock_error = clock_regression.request_command(
        ControllerStationCommandType::OpenLid, 3999U);
    expect(clock_error.reason_code == "MONOTONIC_CLOCK_REGRESSION",
           "a backwards monotonic clock invalidates samples and denies normal commands");
    const auto after_clock_error = clock_regression.request_command(
        ControllerStationCommandType::OpenLid, 4000U);
    expect(after_clock_error.reason_code == "SAFETY_STOP_STATE_UNKNOWN",
           "clock regression requires a fresh safety-stop sample before recovery");
    apply_inputs(clock_regression, safe_closed(), 4000U);
    const auto clock_recovered = clock_regression.request_command(
        ControllerStationCommandType::OpenLid, 4000U);
    expect(clock_recovered.status == ControllerStationCommandDecisionStatus::Allow,
           "fresh post-regression samples restore normal command eligibility");

    ControllerStationState unconfigured;
    telemetry.fill({});
    static_cast<void>(unconfigured.build_telemetry(telemetry, "t5", 5000U));
    const auto* unknown_motion = find(telemetry, "platform.motion_active");
    const auto* unknown_stop = find(telemetry, "safety_stop.active");
    expect(unknown_motion != nullptr &&
               std::get<std::string_view>(unknown_motion->value) == "UNKNOWN" &&
               unknown_motion->quality == SensorQualityStatus::Unknown,
           "never-received motion feedback is UNKNOWN, not stale or false");
    expect(unknown_stop != nullptr &&
               std::get<std::string_view>(unknown_stop->value) == "UNKNOWN" &&
               unknown_stop->quality == SensorQualityStatus::Unknown,
           "never-received safety-stop feedback is UNKNOWN");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
