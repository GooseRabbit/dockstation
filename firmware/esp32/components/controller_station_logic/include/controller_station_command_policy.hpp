#pragma once

#include "lid_limit_switch_tracker.hpp"

#include <cstdint>
#include <string_view>

namespace dockstation::firmware::esp32 {

enum class ControllerStationCommandType : std::uint8_t {
    OpenLid,
    CloseLid,
    StartCleaning,
    StopCleaning,
    RotatePlatform,
    StopMotion
};

enum class ControllerStationCommandDecision : std::uint8_t {
    Allow,
    DenyUnknownLidPosition,
    DenyLidNotClosed,
    DenyLidNotOpen,
    DenyMotionConflict,
    DenyCleaningConflict,
    DenyWorkspaceUnknown,
    DenySafetyStop
};

struct ControllerStationSafetyInputs final {
    LidPositionStatus lid_position{LidPositionStatus::Unknown};
    bool platform_motion_active{false};
    bool cleaning_active{false};
    bool drive_motion_active{false};
    bool workspace_clear{false};
    bool workspace_known{false};
    bool safety_stop_active{false};
};

struct ControllerStationCommandDecision final {
    ControllerStationCommandDecision status{
        ControllerStationCommandDecision::DenyUnknownLidPosition};
    std::string_view reason_code{};
};

[[nodiscard]] ControllerStationCommandDecision evaluate_controller_station_command(
    ControllerStationCommandType command,
    const ControllerStationSafetyInputs& inputs) noexcept;

}  // namespace dockstation::firmware::esp32
