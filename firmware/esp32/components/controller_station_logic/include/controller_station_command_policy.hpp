#pragma once

#include "lid_limit_switch_tracker.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace dockstation::firmware::esp32 {

enum class ControllerStationCommandType : std::uint8_t {
    OpenLid, CloseLid, StartCleaning, StopCleaning, RotatePlatform, StopMotion
};

enum class ControllerStationCommandDecisionStatus : std::uint8_t {
    Allow,
    DenyUnknownLidPosition,
    DenyLidNotClosed,
    DenyLidNotOpen,
    DenyMotionConflict,
    DenyCleaningConflict,
    DenyWorkspaceUnknown,
    DenySafetyStop,
    DenySafetyInputsUnknown,
    DenyCommandPending
};

struct ControllerStationSafetyInputs final {
    LidPositionStatus lid_position{LidPositionStatus::NotConfigured};
    // Missing feedback is not equivalent to an inactive mechanism.
    std::optional<bool> platform_motion_active{};
    std::optional<bool> cleaning_active{};
    std::optional<bool> drive_motion_active{};
    bool workspace_clear{false};
    bool workspace_known{false};
    std::optional<bool> safety_stop_active{};
};

struct ControllerStationCommandDecision final {
    ControllerStationCommandDecisionStatus status{
        ControllerStationCommandDecisionStatus::DenyUnknownLidPosition};
    std::string_view reason_code{};
};

[[nodiscard]] ControllerStationCommandDecision evaluate_controller_station_command(
    ControllerStationCommandType command,
    const ControllerStationSafetyInputs& inputs) noexcept;

}  // namespace dockstation::firmware::esp32
