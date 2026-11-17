#pragma once

#include <cstdint>
#include <optional>

namespace dockstation::firmware::esp32 {

// The electrical level that means a limit switch is active must be selected
// from the verified wiring. Unspecified is deliberately not a usable setting.
enum class LimitSwitchActiveLevel : std::uint8_t {
    Unspecified,
    Low,
    High
};

struct LimitSwitchInputConfig final {
    bool configured{false};
    LimitSwitchActiveLevel active_level{LimitSwitchActiveLevel::Unspecified};
    std::uint64_t debounce_interval_ms{0U};
};

struct LidLimitSwitchConfig final {
    LimitSwitchInputConfig open_limit{};
    LimitSwitchInputConfig closed_limit{};
};

enum class LimitSwitchSignalStatus : std::uint8_t {
    NotConfigured,
    Active,
    Inactive,
    Transitioning,
    Unknown
};

enum class LidPositionStatus : std::uint8_t {
    NotConfigured,
    Open,
    Closed,
    Transitioning,
    Unknown
};

enum class LidLimitSwitchUpdateStatus : std::uint8_t {
    Updated,
    MonotonicTimeWentBackwards
};

struct LidLimitSwitchSnapshot final {
    LimitSwitchSignalStatus open_limit{LimitSwitchSignalStatus::Unknown};
    LimitSwitchSignalStatus closed_limit{LimitSwitchSignalStatus::Unknown};
    LidPositionStatus position{LidPositionStatus::Unknown};
    LidLimitSwitchUpdateStatus update_status{LidLimitSwitchUpdateStatus::Updated};
};

// Pure, allocation-free state tracker for the lid's two end-stop inputs.
// The caller supplies raw electrical levels and a monotonic timestamp; this
// component never accesses GPIO or assumes NO/NC switch wiring.
//
// Both inputs must be configured with an explicit active level and non-zero
// debounce interval before the lid position can be reported as OPEN/CLOSED.
// Two inactive, stable end-stop signals mean the lid is not at either endpoint
// and are reported as TRANSITIONING. Two active signals are contradictory and
// are reported as UNKNOWN.
class LidLimitSwitchTracker final {
public:
    explicit LidLimitSwitchTracker(LidLimitSwitchConfig config = {}) noexcept;

    // Reconfiguration resets debounce history so readings from the old
    // electrical profile cannot be carried into the new profile.
    void configure(LidLimitSwitchConfig config) noexcept;

    // nullopt means that the caller could not obtain a valid input sample.
    // Each input must remain at its candidate level for its own configured
    // interval before it is considered stable.
    [[nodiscard]] LidLimitSwitchSnapshot update(
        std::optional<bool> open_line_high,
        std::optional<bool> closed_line_high,
        std::uint64_t monotonic_time_ms) noexcept;

    void reset() noexcept;

private:
    struct InputState final {
        bool candidate_active{false};
        bool candidate_valid{false};
        std::uint64_t candidate_since_ms{0U};
    };

    [[nodiscard]] static LimitSwitchSignalStatus update_input(
        const LimitSwitchInputConfig& config,
        std::optional<bool> raw_line_high,
        std::uint64_t monotonic_time_ms,
        InputState& state) noexcept;

    [[nodiscard]] static LidPositionStatus classify_position(
        LimitSwitchSignalStatus open_limit,
        LimitSwitchSignalStatus closed_limit) noexcept;

    LidLimitSwitchConfig config_{};
    InputState open_state_{};
    InputState closed_state_{};
    bool has_last_update_time_{false};
    std::uint64_t last_update_time_ms_{0U};
};

}  // namespace dockstation::firmware::esp32
