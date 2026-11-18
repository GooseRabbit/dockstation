#include "controller_station_safety_input_adapter.hpp"

namespace dockstation::firmware::esp32 {

ControllerStationSafetyInputAdapter::ControllerStationSafetyInputAdapter(
    LidLimitSwitchConfig config) noexcept
    : tracker_(config) {}

void ControllerStationSafetyInputAdapter::configure(
    LidLimitSwitchConfig config) noexcept {
    tracker_.configure(config);
    last_snapshot_ = {};
}

ControllerStationSafetyInputs ControllerStationSafetyInputAdapter::update(
    ControllerStationSafetyInputs other_inputs,
    std::optional<bool> open_line_high,
    std::optional<bool> closed_line_high,
    std::uint64_t monotonic_time_ms) noexcept {
    last_snapshot_ = tracker_.update(
        open_line_high, closed_line_high, monotonic_time_ms);
    other_inputs.lid_position = last_snapshot_.position;
    return other_inputs;
}

}  // namespace dockstation::firmware::esp32
