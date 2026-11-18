#include "controller_station_state.hpp"

namespace dockstation::firmware::esp32 {
namespace {

using hardware::interfaces::ControllerSensorValue;
using hardware::interfaces::ControllerTelemetryMeasurement;
using hardware::interfaces::SensorQualityStatus;

[[nodiscard]] bool is_stop_command(ControllerStationCommandType command) noexcept {
    return command == ControllerStationCommandType::StopMotion ||
           command == ControllerStationCommandType::StopCleaning;
}

[[nodiscard]] bool command_requires_lid(ControllerStationCommandType command) noexcept {
    return command == ControllerStationCommandType::OpenLid ||
           command == ControllerStationCommandType::CloseLid ||
           command == ControllerStationCommandType::StartCleaning ||
           command == ControllerStationCommandType::RotatePlatform;
}

[[nodiscard]] std::string_view lid_position_text(
    LidPositionStatus position) noexcept {
    switch (position) {
        case LidPositionStatus::NotConfigured: return "NOT_CONFIGURED";
        case LidPositionStatus::Open: return "OPEN";
        case LidPositionStatus::Closed: return "CLOSED";
        case LidPositionStatus::Transitioning: return "TRANSITIONING";
        case LidPositionStatus::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view decision_text(
    ControllerStationCommandDecisionStatus status) noexcept {
    switch (status) {
        case ControllerStationCommandDecisionStatus::Allow: return "ALLOW";
        case ControllerStationCommandDecisionStatus::DenyUnknownLidPosition:
            return "DENY_UNKNOWN_LID_POSITION";
        case ControllerStationCommandDecisionStatus::DenyLidNotClosed:
            return "DENY_LID_NOT_CLOSED";
        case ControllerStationCommandDecisionStatus::DenyLidNotOpen:
            return "DENY_LID_NOT_OPEN";
        case ControllerStationCommandDecisionStatus::DenyMotionConflict:
            return "DENY_MOTION_CONFLICT";
        case ControllerStationCommandDecisionStatus::DenyCleaningConflict:
            return "DENY_CLEANING_CONFLICT";
        case ControllerStationCommandDecisionStatus::DenyWorkspaceUnknown:
            return "DENY_WORKSPACE_UNKNOWN";
        case ControllerStationCommandDecisionStatus::DenySafetyStop:
            return "DENY_SAFETY_STOP";
        case ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown:
            return "DENY_SAFETY_INPUTS_UNKNOWN";
        case ControllerStationCommandDecisionStatus::DenyCommandPending:
            return "DENY_COMMAND_PENDING";
    }
    return "DENY_UNKNOWN";
}

[[nodiscard]] SensorQualityStatus lid_quality(
    LidPositionStatus position) noexcept {
    switch (position) {
        case LidPositionStatus::Open:
        case LidPositionStatus::Closed:
            return SensorQualityStatus::Ok;
        case LidPositionStatus::NotConfigured:
            return SensorQualityStatus::NotConfigured;
        case LidPositionStatus::Transitioning:
        case LidPositionStatus::Unknown:
            return SensorQualityStatus::Unknown;
    }
    return SensorQualityStatus::Unknown;
}

void append_text_measurement(
    ControllerTelemetryMeasurement& output,
    std::string_view sensor_id,
    std::string_view value,
    SensorQualityStatus quality,
    std::string_view measured_at) noexcept {
    output.sensor_id = sensor_id;
    output.value = ControllerSensorValue{value};
    output.unit = "state";
    output.quality = quality;
    output.measured_at = measured_at;
}

void append_optional_state_measurement(
    ControllerTelemetryMeasurement& output,
    std::string_view sensor_id,
    std::optional<bool> value,
    bool stale,
    std::string_view measured_at) noexcept {
    const std::string_view state = stale
        ? "STALE"
        : (!value.has_value() ? "UNKNOWN" : (*value ? "ACTIVE" : "INACTIVE"));
    const auto quality = stale
        ? SensorQualityStatus::Stale
        : (value.has_value() ? SensorQualityStatus::Ok : SensorQualityStatus::Unknown);
    append_text_measurement(output, sensor_id, state, quality, measured_at);
}

[[nodiscard]] ControllerStationCommandDecision deny(
    ControllerStationCommandDecisionStatus status,
    std::string_view reason) noexcept {
    return {status, reason};
}

}  // namespace

void ControllerStationState::configure_input_freshness(
    ControllerStationInputFreshnessConfig config) noexcept {
    freshness_ = config;
    pending_command_.reset();
    lid_position_received_ = false;
    motion_feedback_received_ = false;
    safety_stop_received_ = false;
    workspace_received_ = false;
    has_last_observed_time_ = false;
    last_observed_time_ms_ = 0U;
    safety_.lid_position = safety_.lid_position == LidPositionStatus::NotConfigured
        ? LidPositionStatus::NotConfigured : LidPositionStatus::Unknown;
    safety_.platform_motion_active.reset();
    safety_.cleaning_active.reset();
    safety_.drive_motion_active.reset();
    safety_.safety_stop_active.reset();
    safety_.workspace_known = false;
    safety_.workspace_clear = false;
    last_command_status_ =
        ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown;
    last_command_reason_ = "INPUT_FRESHNESS_RECONFIGURED";
}

bool ControllerStationState::is_fresh(
    bool received,
    std::uint64_t sampled_at_ms,
    std::uint64_t max_age_ms,
    std::uint64_t now_ms) const noexcept {
    return received && max_age_ms != 0U && now_ms >= sampled_at_ms &&
           now_ms - sampled_at_ms <= max_age_ms;
}

void ControllerStationState::invalidate_samples() noexcept {
    lid_position_received_ = false;
    motion_feedback_received_ = false;
    safety_stop_received_ = false;
    workspace_received_ = false;
    safety_.lid_position = safety_.lid_position == LidPositionStatus::NotConfigured
        ? LidPositionStatus::NotConfigured : LidPositionStatus::Unknown;
    safety_.platform_motion_active.reset();
    safety_.cleaning_active.reset();
    safety_.drive_motion_active.reset();
    safety_.safety_stop_active.reset();
    safety_.workspace_known = false;
    safety_.workspace_clear = false;
    pending_command_.reset();
}

bool ControllerStationState::observe_monotonic_time(
    std::uint64_t now_ms) noexcept {
    if (has_last_observed_time_ && now_ms < last_observed_time_ms_) {
        invalidate_samples();
        last_observed_time_ms_ = now_ms;
        return false;
    }
    has_last_observed_time_ = true;
    last_observed_time_ms_ = now_ms;
    return true;
}

void ControllerStationState::set_safety_inputs(
    ControllerStationSafetyInputs inputs,
    std::uint64_t monotonic_time_ms) noexcept {
    if (!observe_monotonic_time(monotonic_time_ms)) return;
    // Lid position has an independent acquisition cadence and is updated only
    // through update_lid_position(); a cached lid value in this bundle must not
    // refresh its timestamp.
    const LidPositionStatus current_lid_position = safety_.lid_position;
    safety_ = inputs;
    safety_.lid_position = current_lid_position;

    motion_feedback_received_ =
        inputs.platform_motion_active.has_value() &&
        inputs.cleaning_active.has_value() &&
        inputs.drive_motion_active.has_value();
    if (motion_feedback_received_) {
        motion_feedback_sampled_at_ms_ = monotonic_time_ms;
    } else {
        // Treat the motion interlock as one coherent sample set. A partial
        // update must not make the remaining old values look current.
        safety_.platform_motion_active.reset();
        safety_.cleaning_active.reset();
        safety_.drive_motion_active.reset();
    }

    safety_stop_received_ = inputs.safety_stop_active.has_value();
    if (safety_stop_received_) safety_stop_sampled_at_ms_ = monotonic_time_ms;

    workspace_received_ = inputs.workspace_known;
    if (workspace_received_) workspace_sampled_at_ms_ = monotonic_time_ms;
}

void ControllerStationState::update_lid_position(
    LidPositionStatus position,
    std::uint64_t monotonic_time_ms) noexcept {
    if (!observe_monotonic_time(monotonic_time_ms)) return;
    safety_.lid_position = position;
    lid_position_received_ = position != LidPositionStatus::NotConfigured;
    if (lid_position_received_) lid_position_sampled_at_ms_ = monotonic_time_ms;
}

void ControllerStationState::reset_lid_position(LidPositionStatus position) noexcept {
    pending_command_.reset();
    safety_.lid_position = position;
    lid_position_received_ = false;
    last_command_status_ =
        ControllerStationCommandDecisionStatus::DenyUnknownLidPosition;
    last_command_reason_ = "LID_INPUT_RECONFIGURED";
    lid_position_sampled_at_ms_ = 0U;
}

ControllerStationCommandDecision ControllerStationState::request_command(
    ControllerStationCommandType command,
    std::uint64_t monotonic_time_ms) noexcept {
    const bool stop = is_stop_command(command);
    const bool clock_valid = observe_monotonic_time(monotonic_time_ms);
    ControllerStationCommandDecision decision{};

    if (!clock_valid && !stop) {
        decision = deny(
            ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown,
            "MONOTONIC_CLOCK_REGRESSION");
    } else if (pending_command_.has_value() && !stop) {
        decision = deny(
            ControllerStationCommandDecisionStatus::DenyCommandPending,
            "COMMAND_ALREADY_PENDING");
    } else if (!stop &&
               safety_stop_received_ &&
               !is_fresh(safety_stop_received_, safety_stop_sampled_at_ms_,
                         freshness_.safety_stop_max_age_ms, monotonic_time_ms)) {
        decision = deny(
            ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown,
            "SAFETY_STOP_STALE");
    } else if (!stop &&
               motion_feedback_received_ &&
               !is_fresh(motion_feedback_received_, motion_feedback_sampled_at_ms_,
                         freshness_.motion_feedback_max_age_ms, monotonic_time_ms)) {
        decision = deny(
            ControllerStationCommandDecisionStatus::DenySafetyInputsUnknown,
            "MOTION_FEEDBACK_STALE");
    } else if (!stop && command_requires_lid(command) &&
               lid_position_received_ &&
               !is_fresh(lid_position_received_, lid_position_sampled_at_ms_,
                         freshness_.lid_position_max_age_ms, monotonic_time_ms)) {
        decision = deny(
            ControllerStationCommandDecisionStatus::DenyUnknownLidPosition,
            "LID_POSITION_STALE");
    } else if (!stop && command == ControllerStationCommandType::StartCleaning &&
               workspace_received_ &&
               !is_fresh(workspace_received_, workspace_sampled_at_ms_,
                         freshness_.workspace_max_age_ms, monotonic_time_ms)) {
        decision = deny(
            ControllerStationCommandDecisionStatus::DenyWorkspaceUnknown,
            "WORKSPACE_STATE_STALE");
    } else {
        ControllerStationSafetyInputs effective = safety_;
        if (!is_fresh(lid_position_received_, lid_position_sampled_at_ms_,
                      freshness_.lid_position_max_age_ms, monotonic_time_ms) &&
            effective.lid_position != LidPositionStatus::NotConfigured) {
            effective.lid_position = LidPositionStatus::Unknown;
        }
        if (!is_fresh(motion_feedback_received_, motion_feedback_sampled_at_ms_,
                      freshness_.motion_feedback_max_age_ms, monotonic_time_ms)) {
            effective.platform_motion_active.reset();
            effective.cleaning_active.reset();
            effective.drive_motion_active.reset();
        }
        if (!is_fresh(safety_stop_received_, safety_stop_sampled_at_ms_,
                      freshness_.safety_stop_max_age_ms, monotonic_time_ms)) {
            effective.safety_stop_active.reset();
        }
        if (!is_fresh(workspace_received_, workspace_sampled_at_ms_,
                      freshness_.workspace_max_age_ms, monotonic_time_ms)) {
            effective.workspace_known = false;
            effective.workspace_clear = false;
        }
        decision = evaluate_controller_station_command(command, effective);
    }

    last_command_status_ = decision.status;
    last_command_reason_ = decision.reason_code;
    if (decision.status == ControllerStationCommandDecisionStatus::Allow) {
        pending_command_ = command;
    }
    return decision;
}

void ControllerStationState::clear_pending_command() noexcept {
    pending_command_.reset();
}

ControllerStationStateSnapshot ControllerStationState::snapshot() const noexcept {
    return {safety_, pending_command_, last_command_status_, last_command_reason_};
}

std::size_t ControllerStationState::build_telemetry(
    std::array<ControllerTelemetryMeasurement, 8>& output,
    std::string_view measured_at,
    std::uint64_t monotonic_time_ms) noexcept {
    static_cast<void>(observe_monotonic_time(monotonic_time_ms));
    const bool clock_regressed =
        has_last_observed_time_ && monotonic_time_ms < last_observed_time_ms_;
    const bool lid_stale = lid_position_received_ &&
        (clock_regressed ||
         !is_fresh(lid_position_received_, lid_position_sampled_at_ms_,
                   freshness_.lid_position_max_age_ms, monotonic_time_ms));
    const bool motion_stale = motion_feedback_received_ &&
        (clock_regressed ||
         !is_fresh(motion_feedback_received_, motion_feedback_sampled_at_ms_,
                   freshness_.motion_feedback_max_age_ms, monotonic_time_ms));
    const bool stop_stale = safety_stop_received_ &&
        (clock_regressed ||
         !is_fresh(safety_stop_received_, safety_stop_sampled_at_ms_,
                   freshness_.safety_stop_max_age_ms, monotonic_time_ms));
    const bool workspace_stale = workspace_received_ &&
        (clock_regressed ||
         !is_fresh(workspace_received_, workspace_sampled_at_ms_,
                   freshness_.workspace_max_age_ms, monotonic_time_ms));

    append_text_measurement(
        output[0], "lid.position",
        lid_stale ? "STALE" : lid_position_text(safety_.lid_position),
        lid_stale ? SensorQualityStatus::Stale : lid_quality(safety_.lid_position),
        measured_at);
    append_optional_state_measurement(
        output[1], "platform.motion_active", safety_.platform_motion_active,
        motion_stale, measured_at);
    append_optional_state_measurement(
        output[2], "cleaning.active", safety_.cleaning_active,
        motion_stale, measured_at);
    append_optional_state_measurement(
        output[3], "drive.motion_active", safety_.drive_motion_active,
        motion_stale, measured_at);
    append_optional_state_measurement(
        output[4], "safety_stop.active", safety_.safety_stop_active,
        stop_stale, measured_at);

    const bool workspace_known = safety_.workspace_known && !workspace_stale;
    append_text_measurement(
        output[5], "workspace.state",
        workspace_stale ? "STALE" :
            (workspace_known ? (safety_.workspace_clear ? "CLEAR" : "NOT_CLEAR")
                             : "UNKNOWN"),
        workspace_stale ? SensorQualityStatus::Stale :
            (workspace_known ? SensorQualityStatus::Ok : SensorQualityStatus::Unknown),
        measured_at);

    append_text_measurement(
        output[6], "command.last_decision",
        decision_text(last_command_status_),
        last_command_reason_.empty()
            ? SensorQualityStatus::Unknown
            : SensorQualityStatus::Ok,
        measured_at);

    append_text_measurement(
        output[7], "command.pending",
        pending_command_.has_value() ? "PENDING" : "NONE",
        SensorQualityStatus::Ok, measured_at);

    return output.size();
}

}  // namespace dockstation::firmware::esp32
