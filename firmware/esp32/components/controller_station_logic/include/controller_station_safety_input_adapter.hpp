#pragma once

#include "controller_station_command_policy.hpp"

#include <cstdint>
#include <optional>

namespace dockstation::firmware::esp32 {

// Converts debounced raw lid end-stop samples into the safety input consumed
// by ControllerStationState. GPIO acquisition and wiring polarity stay outside.
class ControllerStationSafetyInputAdapter final {
public:
    explicit ControllerStationSafetyInputAdapter(
        LidLimitSwitchConfig config = {}) noexcept;

    void configure(LidLimitSwitchConfig config) noexcept;

    [[nodiscard]] ControllerStationSafetyInputs update(
        ControllerStationSafetyInputs other_inputs,
        std::optional<bool> open_line_high,
        std::optional<bool> closed_line_high,
        std::uint64_t monotonic_time_ms) noexcept;

    [[nodiscard]] LidLimitSwitchSnapshot last_lid_snapshot() const noexcept {
        return last_snapshot_;
    }

private:
    LidLimitSwitchTracker tracker_{};
    LidLimitSwitchSnapshot last_snapshot_{};
};

}  // namespace dockstation::firmware::esp32
