#include "lid_limit_switch_tracker.hpp"

namespace dockstation::firmware::esp32 {
namespace {

[[nodiscard]] bool is_valid_config(
    const LimitSwitchInputConfig& config) noexcept {
    return config.configured &&
           config.debounce_interval_ms > 0U &&
           (config.active_level == LimitSwitchActiveLevel::Low ||
            config.active_level == LimitSwitchActiveLevel::High);
}

}  // namespace

LidLimitSwitchTracker::LidLimitSwitchTracker(
    LidLimitSwitchConfig config) noexcept
    : config_(config) {}

void LidLimitSwitchTracker::configure(LidLimitSwitchConfig config) noexcept {
    config_ = config;
    reset();
}

void LidLimitSwitchTracker::reset() noexcept {
    open_state_ = {};
    closed_state_ = {};
    has_last_update_time_ = false;
    last_update_time_ms_ = 0U;
}

LimitSwitchSignalStatus LidLimitSwitchTracker::update_input(
    const LimitSwitchInputConfig& config,
    std::optional<bool> raw_line_high,
    std::uint64_t monotonic_time_ms,
    InputState& state) noexcept {
    if (!config.configured) {
        state = {};
        return LimitSwitchSignalStatus::NotConfigured;
    }

    if (!is_valid_config(config) || !raw_line_high.has_value()) {
        state = {};
        return LimitSwitchSignalStatus::Unknown;
    }

    const bool active_level_high =
        config.active_level == LimitSwitchActiveLevel::High;
    const bool candidate_active = *raw_line_high == active_level_high;

    if (!state.candidate_valid ||
        state.candidate_active != candidate_active) {
        state.candidate_active = candidate_active;
        state.candidate_since_ms = monotonic_time_ms;
        state.candidate_valid = true;
        return LimitSwitchSignalStatus::Transitioning;
    }

    if (monotonic_time_ms - state.candidate_since_ms <
        config.debounce_interval_ms) {
        return LimitSwitchSignalStatus::Transitioning;
    }

    return candidate_active
        ? LimitSwitchSignalStatus::Active
        : LimitSwitchSignalStatus::Inactive;
}

LidPositionStatus LidLimitSwitchTracker::classify_position(
    LimitSwitchSignalStatus open_limit,
    LimitSwitchSignalStatus closed_limit) noexcept {
    if (open_limit == LimitSwitchSignalStatus::NotConfigured &&
        closed_limit == LimitSwitchSignalStatus::NotConfigured) {
        return LidPositionStatus::NotConfigured;
    }

    if (open_limit == LimitSwitchSignalStatus::NotConfigured ||
        closed_limit == LimitSwitchSignalStatus::NotConfigured ||
        open_limit == LimitSwitchSignalStatus::Unknown ||
        closed_limit == LimitSwitchSignalStatus::Unknown) {
        return LidPositionStatus::Unknown;
    }

    if (open_limit == LimitSwitchSignalStatus::Transitioning ||
        closed_limit == LimitSwitchSignalStatus::Transitioning) {
        return LidPositionStatus::Transitioning;
    }

    if (open_limit == LimitSwitchSignalStatus::Active &&
        closed_limit == LimitSwitchSignalStatus::Inactive) {
        return LidPositionStatus::Open;
    }

    if (open_limit == LimitSwitchSignalStatus::Inactive &&
        closed_limit == LimitSwitchSignalStatus::Active) {
        return LidPositionStatus::Closed;
    }

    if (open_limit == LimitSwitchSignalStatus::Active &&
        closed_limit == LimitSwitchSignalStatus::Active) {
        return LidPositionStatus::Unknown;
    }

    // Neither end-stop is active: the lid is not at a verified endpoint.
    return LidPositionStatus::Transitioning;
}

LidLimitSwitchSnapshot LidLimitSwitchTracker::update(
    std::optional<bool> open_line_high,
    std::optional<bool> closed_line_high,
    std::uint64_t monotonic_time_ms) noexcept {
    LidLimitSwitchSnapshot snapshot{};

    if (has_last_update_time_ &&
        monotonic_time_ms < last_update_time_ms_) {
        open_state_ = {};
        closed_state_ = {};
        snapshot.open_limit =
            config_.open_limit.configured
                ? LimitSwitchSignalStatus::Unknown
                : LimitSwitchSignalStatus::NotConfigured;
        snapshot.closed_limit =
            config_.closed_limit.configured
                ? LimitSwitchSignalStatus::Unknown
                : LimitSwitchSignalStatus::NotConfigured;
        snapshot.position = classify_position(
            snapshot.open_limit, snapshot.closed_limit);
        snapshot.update_status =
            LidLimitSwitchUpdateStatus::MonotonicTimeWentBackwards;
        last_update_time_ms_ = monotonic_time_ms;
        return snapshot;
    }

    has_last_update_time_ = true;
    last_update_time_ms_ = monotonic_time_ms;

    snapshot.open_limit = update_input(
        config_.open_limit, open_line_high, monotonic_time_ms, open_state_);
    snapshot.closed_limit = update_input(
        config_.closed_limit, closed_line_high, monotonic_time_ms, closed_state_);
    snapshot.position = classify_position(
        snapshot.open_limit, snapshot.closed_limit);
    return snapshot;
}

}  // namespace dockstation::firmware::esp32
