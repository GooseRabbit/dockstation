#pragma once

#include "controller_station_command_policy.hpp"
#include "hardware/interfaces/controller_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dockstation::firmware::esp32 {

// Freshness windows are deliberately explicit. Zero disables trust for that
// input class until a verified hardware profile supplies a measured interval.
struct ControllerStationInputFreshnessConfig final {
    std::uint64_t lid_position_max_age_ms{0U};
    std::uint64_t motion_feedback_max_age_ms{0U};
    std::uint64_t safety_stop_max_age_ms{0U};
    std::uint64_t workspace_max_age_ms{0U};
};

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

    // Reconfiguration invalidates prior samples; each source must report again.
    void configure_input_freshness(
        ControllerStationInputFreshnessConfig config) noexcept;

    // Updates motion, safety-stop and workspace feedback. Lid position has
    // an independent acquisition cadence and must use update_lid_position().
    // Missing optional values and unknown workspace state stay missing.
    void set_safety_inputs(
        ControllerStationSafetyInputs inputs,
        std::uint64_t monotonic_time_ms) noexcept;

    // End-stop samples have their own acquisition cadence and timestamp.
    void update_lid_position(
        LidPositionStatus position,
        std::uint64_t monotonic_time_ms) noexcept;
    void reset_lid_position(LidPositionStatus position) noexcept;

    [[nodiscard]] ControllerStationCommandDecision request_command(
        ControllerStationCommandType command,
        std::uint64_t monotonic_time_ms) noexcept;

    void clear_pending_command() noexcept;

    [[nodiscard]] ControllerStationStateSnapshot snapshot() const noexcept;

    [[nodiscard]] std::size_t build_telemetry(
        std::array<hardware::interfaces::ControllerTelemetryMeasurement, 8>& output,
        std::string_view measured_at,
        std::uint64_t monotonic_time_ms) noexcept;

private:
    [[nodiscard]] bool is_fresh(
        bool received,
        std::uint64_t sampled_at_ms,
        std::uint64_t max_age_ms,
        std::uint64_t now_ms) const noexcept;
    [[nodiscard]] bool observe_monotonic_time(std::uint64_t now_ms) noexcept;
    void invalidate_samples() noexcept;

    ControllerStationSafetyInputs safety_{};
    ControllerStationInputFreshnessConfig freshness_{};
    std::uint64_t lid_position_sampled_at_ms_{0U};
    std::uint64_t motion_feedback_sampled_at_ms_{0U};
    std::uint64_t safety_stop_sampled_at_ms_{0U};
    std::uint64_t workspace_sampled_at_ms_{0U};
    bool lid_position_received_{false};
    bool motion_feedback_received_{false};
    bool safety_stop_received_{false};
    bool workspace_received_{false};
    bool has_last_observed_time_{false};
    std::uint64_t last_observed_time_ms_{0U};
    std::optional<ControllerStationCommandType> pending_command_{};
    ControllerStationCommandDecisionStatus last_command_status_{
        ControllerStationCommandDecisionStatus::DenyUnknownLidPosition};
    std::string_view last_command_reason_{};
};

}  // namespace dockstation::firmware::esp32
