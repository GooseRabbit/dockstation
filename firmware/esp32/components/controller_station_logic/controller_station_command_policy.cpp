#include "controller_station_command_policy.hpp"

namespace dockstation::firmware::esp32 {
namespace {

[[nodiscard]] ControllerStationCommandDecision deny(
    ControllerStationCommandDecision status,
    std::string_view reason) noexcept {
    return {status, reason};
}

[[nodiscard]] ControllerStationCommandDecision allow() noexcept {
    return {ControllerStationCommandDecision::Allow, "ALLOWED"};
}

[[nodiscard]] bool any_motion(
    const ControllerStationSafetyInputs& inputs) noexcept {
    return inputs.platform_motion_active || inputs.drive_motion_active;
}

}  // namespace

ControllerStationCommandDecision evaluate_controller_station_command(
    ControllerStationCommandType command,
    const ControllerStationSafetyInputs& inputs) noexcept {
    // Stop commands are deliberately fail-safe: an unknown lid position must
    // never prevent a request to stop an already running mechanism.
    if (command == ControllerStationCommandType::StopMotion ||
        command == ControllerStationCommandType::StopCleaning) {
        return allow();
    }

    if (inputs.safety_stop_active) {
        return deny(
            ControllerStationCommandDecision::DenySafetyStop,
            "SAFETY_STOP_ACTIVE");
    }

    switch (command) {
        case ControllerStationCommandType::OpenLid:
            if (inputs.lid_position == LidPositionStatus::Unknown ||
                inputs.lid_position == LidPositionStatus::NotConfigured) {
                return deny(
                    ControllerStationCommandDecision::DenyUnknownLidPosition,
                    "LID_POSITION_UNKNOWN");
            }
            if (inputs.lid_position != LidPositionStatus::Closed) {
                return deny(
                    ControllerStationCommandDecision::DenyLidNotClosed,
                    "LID_NOT_CLOSED");
            }
            if (any_motion(inputs) || inputs.cleaning_active) {
                return deny(
                    ControllerStationCommandDecision::DenyMotionConflict,
                    "MOTION_ACTIVE");
            }
            return allow();

        case ControllerStationCommandType::CloseLid:
            if (inputs.lid_position == LidPositionStatus::Unknown ||
                inputs.lid_position == LidPositionStatus::NotConfigured) {
                return deny(
                    ControllerStationCommandDecision::DenyUnknownLidPosition,
                    "LID_POSITION_UNKNOWN");
            }
            if (inputs.lid_position != LidPositionStatus::Open) {
                return deny(
                    ControllerStationCommandDecision::DenyLidNotOpen,
                    "LID_NOT_OPEN");
            }
            if (any_motion(inputs) || inputs.cleaning_active) {
                return deny(
                    ControllerStationCommandDecision::DenyMotionConflict,
                    "MOTION_ACTIVE");
            }
            return allow();

        case ControllerStationCommandType::StartCleaning:
            if (inputs.lid_position != LidPositionStatus::Closed) {
                return deny(
                    inputs.lid_position == LidPositionStatus::Unknown ||
                            inputs.lid_position == LidPositionStatus::NotConfigured
                        ? ControllerStationCommandDecision::DenyUnknownLidPosition
                        : ControllerStationCommandDecision::DenyLidNotClosed,
                    inputs.lid_position == LidPositionStatus::Unknown ||
                            inputs.lid_position == LidPositionStatus::NotConfigured
                        ? "LID_POSITION_UNKNOWN"
                        : "LID_NOT_CLOSED");
            }
            if (inputs.drive_motion_active) {
                return deny(
                    ControllerStationCommandDecision::DenyMotionConflict,
                    "DRIVE_MOTION_ACTIVE");
            }
            if (inputs.platform_motion_active || inputs.cleaning_active) {
                return deny(
                    ControllerStationCommandDecision::DenyCleaningConflict,
                    "CLEANING_OR_PLATFORM_ACTIVE");
            }
            if (!inputs.workspace_known || !inputs.workspace_clear) {
                return deny(
                    ControllerStationCommandDecision::DenyWorkspaceUnknown,
                    "WORKSPACE_NOT_CONFIRMED_CLEAR");
            }
            return allow();

        case ControllerStationCommandType::RotatePlatform:
            if (inputs.lid_position != LidPositionStatus::Closed) {
                return deny(
                    inputs.lid_position == LidPositionStatus::Unknown ||
                            inputs.lid_position == LidPositionStatus::NotConfigured
                        ? ControllerStationCommandDecision::DenyUnknownLidPosition
                        : ControllerStationCommandDecision::DenyLidNotClosed,
                    inputs.lid_position == LidPositionStatus::Unknown ||
                            inputs.lid_position == LidPositionStatus::NotConfigured
                        ? "LID_POSITION_UNKNOWN"
                        : "LID_NOT_CLOSED");
            }
            if (inputs.cleaning_active || inputs.drive_motion_active) {
                return deny(
                    ControllerStationCommandDecision::DenyMotionConflict,
                    "CONFLICTING_OPERATION_ACTIVE");
            }
            return allow();

        case ControllerStationCommandType::StopMotion:
        case ControllerStationCommandType::StopCleaning:
            return allow();
    }

    return deny(
        ControllerStationCommandDecision::DenySafetyStop,
        "UNKNOWN_COMMAND");
}

}  // namespace dockstation::firmware::esp32
