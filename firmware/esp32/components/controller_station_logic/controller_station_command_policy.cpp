#include "controller_station_command_policy.hpp"

namespace dockstation::firmware::esp32 {
namespace {
[[nodiscard]] ControllerStationCommandDecision deny(
    ControllerStationCommandDecisionStatus status, std::string_view reason) noexcept {
    return {status, reason};
}
[[nodiscard]] ControllerStationCommandDecision allow() noexcept {
    return {ControllerStationCommandDecisionStatus::Allow, "ALLOWED"};
}
[[nodiscard]] bool is_stop_command(ControllerStationCommandType command) noexcept {
    return command == ControllerStationCommandType::StopMotion ||
           command == ControllerStationCommandType::StopCleaning;
}
[[nodiscard]] bool motion_feedback_complete(
    const ControllerStationSafetyInputs& inputs) noexcept {
    return inputs.platform_motion_active.has_value() &&
           inputs.cleaning_active.has_value() &&
           inputs.drive_motion_active.has_value();
}
[[nodiscard]] bool any_motion(const ControllerStationSafetyInputs& inputs) noexcept {
    return *inputs.platform_motion_active || *inputs.drive_motion_active ||
           *inputs.cleaning_active;
}
}  // namespace

ControllerStationCommandDecision evaluate_controller_station_command(
    ControllerStationCommandType command,
    const ControllerStationSafetyInputs& inputs) noexcept {
    // Stop requests remain available even when feedback is missing or a safety
    // stop is active. This is authorization only, not motor actuation.
    if (is_stop_command(command)) return allow();

    if (!inputs.safety_stop_active.has_value()) {
        return deny(ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown,
                    "SAFETY_STOP_STATE_UNKNOWN");
    }
    if (*inputs.safety_stop_active) {
        return deny(ControllerStationCommandDecisionStatus::DenySafetyStop,
                    "SAFETY_STOP_ACTIVE");
    }
    if (!motion_feedback_complete(inputs)) {
        return deny(ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown,
                    "MOTION_STATE_UNKNOWN");
    }

    switch (command) {
        case ControllerStationCommandType::OpenLid:
            if (inputs.lid_position == LidPositionStatus::Unknown ||
                inputs.lid_position == LidPositionStatus::NotConfigured)
                return deny(ControllerStationCommandDecisionStatus::DenyUnknownLidPosition,
                            "LID_POSITION_UNKNOWN");
            if (inputs.lid_position != LidPositionStatus::Closed)
                return deny(ControllerStationCommandDecisionStatus::DenyLidNotClosed,
                            "LID_NOT_CLOSED");
            if (any_motion(inputs))
                return deny(ControllerStationCommandDecisionStatus::DenyMotionConflict,
                            "MOTION_ACTIVE");
            return allow();

        case ControllerStationCommandType::CloseLid:
            if (inputs.lid_position == LidPositionStatus::Unknown ||
                inputs.lid_position == LidPositionStatus::NotConfigured)
                return deny(ControllerStationCommandDecisionStatus::DenyUnknownLidPosition,
                            "LID_POSITION_UNKNOWN");
            if (inputs.lid_position != LidPositionStatus::Open)
                return deny(ControllerStationCommandDecisionStatus::DenyLidNotOpen,
                            "LID_NOT_OPEN");
            if (any_motion(inputs))
                return deny(ControllerStationCommandDecisionStatus::DenyMotionConflict,
                            "MOTION_ACTIVE");
            return allow();

        case ControllerStationCommandType::StartCleaning:
            if (inputs.lid_position != LidPositionStatus::Closed)
                return deny(
                    inputs.lid_position == LidPositionStatus::Unknown ||
                            inputs.lid_position == LidPositionStatus::NotConfigured
                        ? ControllerStationCommandDecisionStatus::DenyUnknownLidPosition
                        : ControllerStationCommandDecisionStatus::DenyLidNotClosed,
                    inputs.lid_position == LidPositionStatus::Unknown ||
                            inputs.lid_position == LidPositionStatus::NotConfigured
                        ? "LID_POSITION_UNKNOWN" : "LID_NOT_CLOSED");
            if (*inputs.drive_motion_active)
                return deny(ControllerStationCommandDecisionStatus::DenyMotionConflict,
                            "DRIVE_MOTION_ACTIVE");
            if (*inputs.platform_motion_active || *inputs.cleaning_active)
                return deny(ControllerStationCommandDecisionStatus::DenyCleaningConflict,
                            "CLEANING_OR_PLATFORM_ACTIVE");
            if (!inputs.workspace_known || !inputs.workspace_clear)
                return deny(ControllerStationCommandDecisionStatus::DenyWorkspaceUnknown,
                            "WORKSPACE_NOT_CONFIRMED_CLEAR");
            return allow();

        case ControllerStationCommandType::RotatePlatform:
            if (inputs.lid_position != LidPositionStatus::Closed)
                return deny(
                    inputs.lid_position == LidPositionStatus::Unknown ||
                            inputs.lid_position == LidPositionStatus::NotConfigured
                        ? ControllerStationCommandDecisionStatus::DenyUnknownLidPosition
                        : ControllerStationCommandDecisionStatus::DenyLidNotClosed,
                    inputs.lid_position == LidPositionStatus::Unknown ||
                            inputs.lid_position == LidPositionStatus::NotConfigured
                        ? "LID_POSITION_UNKNOWN" : "LID_NOT_CLOSED");
            if (any_motion(inputs))
                return deny(ControllerStationCommandDecisionStatus::DenyMotionConflict,
                            "CONFLICTING_OPERATION_ACTIVE");
            return allow();

        case ControllerStationCommandType::StopMotion:
        case ControllerStationCommandType::StopCleaning:
            return allow();
    }
    return deny(ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown,
                "UNKNOWN_COMMAND");
}
}  // namespace dockstation::firmware::esp32
