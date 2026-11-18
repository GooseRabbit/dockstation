#pragma once

#include "controller_station_command_policy.hpp"
#include "hardware/interfaces/controller_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dockstation::firmware::esp32 {

struct ControllerStationStateSnapshot final {
    ControllerStationSafetyInputs safety{};
    std::optional<ControllerStationCommandType> pending_command{};
    ControllerStationCommandDecisionStatus last_command_status{
        ControllerStationCommandDecisionStatus::DenyUnknownLidPosition};
    std::string_view last_command_reason{};
};

class ControllerStationState final {
public:
    ControllerStationState() noexcept = default;

    void set_safety_inputs(ControllerStationSafetyInputs inputs) noexcept;

    [[nodiscard]] ControllerStationCommandDecision request_command(
        ControllerStationCommandType command) noexcept;

    void clear_pending_command() noexcept;

    [[nodiscard]] ControllerStationStateSnapshot snapshot() const noexcept;

    [[nodiscard]] std::size_t build_telemetry(
        std::array<hardware::interfaces::ControllerTelemetryMeasurement, 8>& output,
        std::string_view measured_at) const noexcept;

private:
    ControllerStationSafetyInputs safety_{};
    std::optional<ControllerStationCommandType> pending_command_{};
    ControllerStationCommandDecisionStatus last_command_status_{
        ControllerStationCommandDecisionStatus::DenyUnknownLidPosition};
    std::string_view last_command_reason_{};
};

}  // namespace dockstation::firmware::esp32
