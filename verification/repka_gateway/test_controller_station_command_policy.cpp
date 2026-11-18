#include "controller_station_command_policy.hpp"

#include <cstdlib>
#include <iostream>

using dockstation::firmware::esp32::ControllerStationCommandDecisionStatus;
using dockstation::firmware::esp32::ControllerStationCommandType;
using dockstation::firmware::esp32::ControllerStationSafetyInputs;
using dockstation::firmware::esp32::LidPositionStatus;
using dockstation::firmware::esp32::evaluate_controller_station_command;

namespace {
int failures = 0;

void expect(bool condition, const char* description) {
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        std::cerr << "FAIL - " << description << '\n';
        ++failures;
    }
}
}  // namespace

int main() {
    ControllerStationSafetyInputs safe_closed{};
    safe_closed.lid_position = LidPositionStatus::Closed;
    safe_closed.workspace_known = true;
    safe_closed.workspace_clear = true;

    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::OpenLid, safe_closed).status ==
            ControllerStationCommandDecisionStatus::Allow,
        "open lid is allowed only from a confirmed closed position");

    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::StartCleaning, safe_closed).status ==
            ControllerStationCommandDecisionStatus::Allow,
        "cleaning requires a confirmed clear workspace");

    safe_closed.workspace_clear = false;
    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::StartCleaning, safe_closed).status ==
            ControllerStationCommandDecisionStatus::DenyWorkspaceUnknown,
        "cleaning is denied when the workspace is not confirmed clear");

    ControllerStationSafetyInputs unknown_lid = safe_closed;
    unknown_lid.lid_position = LidPositionStatus::Unknown;
    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::OpenLid, unknown_lid).status ==
            ControllerStationCommandDecisionStatus::DenyUnknownLidPosition,
        "unknown lid position never permits opening");

    ControllerStationSafetyInputs open_lid{};
    open_lid.lid_position = LidPositionStatus::Open;
    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::CloseLid, open_lid).status ==
            ControllerStationCommandDecisionStatus::Allow,
        "close lid is allowed only from a confirmed open position");

    open_lid.platform_motion_active = true;
    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::CloseLid, open_lid).status ==
            ControllerStationCommandDecisionStatus::DenyMotionConflict,
        "lid close is denied while another mechanism is moving");

    open_lid.safety_stop_active = true;
    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::CloseLid, open_lid).status ==
            ControllerStationCommandDecisionStatus::DenySafetyStop,
        "safety stop denies normal motion commands");

    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::StopMotion, unknown_lid).status ==
            ControllerStationCommandDecisionStatus::Allow,
        "stop motion remains allowed with unknown lid position");

    expect(
        evaluate_controller_station_command(
            ControllerStationCommandType::StopCleaning, unknown_lid).status ==
            ControllerStationCommandDecisionStatus::Allow,
        "stop cleaning remains allowed with unknown lid position");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
