#include "controller_station_state.hpp"

namespace dockstation::firmware::esp32 {
namespace {

using hardware::interfaces::ControllerSensorValue;
using hardware::interfaces::ControllerTelemetryMeasurement;
using hardware::interfaces::SensorQualityStatus;

[[nodiscard]] std::string_view lid_position_text(
    LidPositionStatus position) noexcept {
    switch (position) {
        case LidPositionStatus::NotConfigured: return "NOT_CONFIGURED";
        case LidPositionStatus::Open: return "OPEN";
        case LidPositionStatus::Closed: return "CLOSED";
        case LidPositionStatus::Transitioning: return "TRANSITIONING";
        case LidPositionStatus::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view decision_text(
    ControllerStationCommandDecisionStatus status) noexcept {
    switch (status) {
        case ControllerStationCommandDecisionStatus::Allow: return "ALLOW";
        case ControllerStationCommandDecisionStatus::DenyUnknownLidPosition:
            return "DENY_UNKNOWN_LID_POSITION";
        case ControllerStationCommandDecisionStatus::DenyLidNotClosed:
            return "DENY_LID_NOT_CLOSED";
        case ControllerStationCommandDecisionStatus::DenyLidNotOpen:
            return "DENY_LID_NOT_OPEN";
        case ControllerStationCommandDecisionStatus::DenyMotionConflict:
            return "DENY_MOTION_CONFLICT";
        case ControllerStationCommandDecisionStatus::DenyCleaningConflict:
            return "DENY_CLEANING_CONFLICT";
        case ControllerStationCommandDecisionStatus::DenyWorkspaceUnknown:
            return "DENY_WORKSPACE_UNKNOWN";
        case ControllerStationCommandDecisionStatus::DenySafetyStop:
            return "DENY_SAFETY_STOP";
        case ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown:
            return "DENY_SAFETY_INPUTS_UNKNOWN";
        case ControllerStationCommandDecisionStatus::DenyCommandPending:
            return "DENY_COMMAND_PENDING";
    }
    return "DENY_UNKNOWN";
}

[[nodiscard]] SensorQualityStatus lid_quality(
    LidPositionStatus position) noexcept {
    switch (position) {
        case LidPositionStatus::Open:
        case LidPositionStatus::Closed:
            return SensorQualityStatus::Ok;
        case LidPositionStatus::NotConfigured:
            return SensorQualityStatus::NotConfigured;
        case LidPositionStatus::Transitioning:
        case LidPositionStatus::Unknown:
            return SensorQualityStatus::Unknown;
    }
    return SensorQualityStatus::Unknown;
}

void append_text_measurement(
    ControllerTelemetryMeasurement& output,
    std::string_view sensor_id,
    std::string_view value,
    SensorQualityStatus quality,
    std::string_view measured_at) noexcept {
    output.sensor_id = sensor_id;
    output.value = ControllerSensorValue{value};
    output.unit = "state";
    output.quality = quality;
    output.measured_at = measured_at;
}

void append_optional_state_measurement(
    ControllerTelemetryMeasurement& output,
    std::string_view sensor_id,
    std::optional<bool> value,
    std::string_view measured_at) noexcept {
    const std::string_view state = !value.has_value()
        ? "UNKNOWN" : (*value ? "ACTIVE" : "INACTIVE");
    append_text_measurement(
        output, sensor_id, state,
        value.has_value() ? SensorQualityStatus::Ok : SensorQualityStatus::Unknown,
        measured_at);
}

}  // namespace

void ControllerStationState::set_safety_inputs(
    ControllerStationSafetyInputs inputs) noexcept {
    safety_ = inputs;
}

ControllerStationCommandDecision ControllerStationState::request_command(
    ControllerStationCommandType command) noexcept {
    const bool is_stop =
        command == ControllerStationCommandType::StopMotion ||
        command == ControllerStationCommandType::StopCleaning;
    // An accepted command remains pending until the actuator layer confirms
    // completion. A later normal command must not silently replace it.
    const auto decision = pending_command_.has_value() && !is_stop
        ? ControllerStationCommandDecision{
              ControllerStationCommandDecisionStatus::DenyCommandPending,
              "COMMAND_ALREADY_PENDING"}
        : evaluate_controller_station_command(command, safety_);
    last_command_status_ = decision.status;
    last_command_reason_ = decision.reason_code;
    if (decision.status == ControllerStationCommandDecisionStatus::Allow) {
        // A stop request may supersede a pending motion request.
        pending_command_ = command;
    }
    return decision;
}

void ControllerStationState::clear_pending_command() noexcept {
    pending_command_.reset();
}

ControllerStationStateSnapshot ControllerStationState::snapshot() const noexcept {
    return {safety_, pending_command_, last_command_status_, last_command_reason_};
}

std::size_t ControllerStationState::build_telemetry(
    std::array<ControllerTelemetryMeasurement, 8>& output,
    std::string_view measured_at) const noexcept {
    append_text_measurement(
        output[0], "lid.position", lid_position_text(safety_.lid_position),
        lid_quality(safety_.lid_position), measured_at);
    append_optional_state_measurement(
        output[1], "platform.motion_active", safety_.platform_motion_active, measured_at);
    append_optional_state_measurement(
        output[2], "cleaning.active", safety_.cleaning_active, measured_at);
    append_optional_state_measurement(
        output[3], "drive.motion_active", safety_.drive_motion_active, measured_at);
    append_optional_state_measurement(
        output[4], "safety_stop.active", safety_.safety_stop_active, measured_at);

    append_text_measurement(
        output[5], "workspace.state",
        safety_.workspace_known
            ? (safety_.workspace_clear ? "CLEAR" : "NOT_CLEAR")
            : "UNKNOWN",
        safety_.workspace_known
            ? SensorQualityStatus::Ok
            : SensorQualityStatus::Unknown,
        measured_at);

    append_text_measurement(
        output[6], "command.last_decision",
        decision_text(last_command_status_),
        last_command_reason_.empty()
            ? SensorQualityStatus::Unknown
            : SensorQualityStatus::Ok,
        measured_at);

    append_text_measurement(
        output[7], "command.pending",
        pending_command_.has_value() ? "PENDING" : "NONE",
        SensorQualityStatus::Ok, measured_at);

    return output.size();
}

}  // namespace dockstation::firmware::esp32
