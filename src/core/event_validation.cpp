#include "event_validation.hpp"

#include <array>

namespace dockstation::core {
namespace {

constexpr EventValidationResult valid_result() noexcept {
    return {true, EventValidationError::None, {}, "Event envelope is valid"};
}

constexpr EventValidationResult invalid_result(
    EventValidationError error,
    std::string_view field,
    std::string_view message) noexcept {
    return {false, error, field, message};
}

constexpr bool is_ascii_whitespace(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
           ch == '\f' || ch == '\v';
}

bool has_non_whitespace(std::string_view value) noexcept {
    for (const char ch : value) {
        if (!is_ascii_whitespace(ch)) {
            return true;
        }
    }
    return false;
}

bool is_known_quality_status(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 6> allowed{
        "VALID", "STALE", "INVALID", "UNAVAILABLE", "UNKNOWN", "ERROR"};
    for (const auto candidate : allowed) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

}  // namespace

EventValidationResult validate_event(
    const Event& event,
    const EventValidationPolicy& policy) noexcept {
    // Required envelope fields. The function returns the first error in this
    // stable order so callers and tests receive deterministic diagnostics.
    if (!has_non_whitespace(event.event_id)) {
        return invalid_result(EventValidationError::MissingEventId,
                              "event_id", "event_id is required");
    }
    if (!has_non_whitespace(event.event_type)) {
        return invalid_result(EventValidationError::MissingEventType,
                              "event_type", "event_type is required");
    }
    if (!has_non_whitespace(event.source)) {
        return invalid_result(EventValidationError::MissingSource,
                              "source", "source is required");
    }
    if (!has_non_whitespace(event.received_at)) {
        return invalid_result(EventValidationError::MissingReceivedAt,
                              "received_at", "received_at is required");
    }
    if (!event.payload.has_value()) {
        return invalid_result(EventValidationError::MissingPayload,
                              "payload", "payload must be present");
    }
    if (!has_non_whitespace(event.quality_status)) {
        return invalid_result(EventValidationError::MissingQualityStatus,
                              "quality_status", "quality_status is required");
    }
    if (!is_known_quality_status(event.quality_status)) {
        return invalid_result(EventValidationError::UnknownQualityStatus,
                              "quality_status", "quality_status is not supported");
    }
    if (!has_non_whitespace(event.schema_version)) {
        return invalid_result(EventValidationError::MissingSchemaVersion,
                              "schema_version", "schema_version is required");
    }

    // Optional values must not be present-but-empty.
    if (event.occurred_at.has_value() &&
        !has_non_whitespace(*event.occurred_at)) {
        return invalid_result(EventValidationError::EmptyOccurredAt,
                              "occurred_at", "occurred_at must not be empty when present");
    }
    if (event.source_instance.has_value() &&
        !has_non_whitespace(*event.source_instance)) {
        return invalid_result(EventValidationError::EmptySourceInstance,
                              "source_instance", "source_instance must not be empty when present");
    }
    if (policy.require_source_instance && !event.source_instance.has_value()) {
        return invalid_result(EventValidationError::MissingSourceInstance,
                              "source_instance", "source_instance is required by source policy");
    }
    if (policy.require_source_sequence && !event.source_sequence.has_value()) {
        return invalid_result(EventValidationError::MissingSourceSequence,
                              "source_sequence", "source_sequence is required by source policy");
    }

    const bool has_operation_id = event.operation_id.has_value();
    const bool has_correlation_id = event.correlation_id.has_value();
    if (has_operation_id != has_correlation_id) {
        return invalid_result(EventValidationError::PartialOperationContext,
                              "operation_id/correlation_id",
                              "operation_id and correlation_id must be supplied together");
    }
    if (policy.require_operation_context && !has_operation_id) {
        return invalid_result(EventValidationError::MissingOperationContext,
                              "operation_id/correlation_id",
                              "operation_id and correlation_id are required by event policy");
    }
    if (has_operation_id && !has_non_whitespace(*event.operation_id)) {
        return invalid_result(EventValidationError::EmptyOperationId,
                              "operation_id", "operation_id must not be empty when present");
    }
    if (has_correlation_id && !has_non_whitespace(*event.correlation_id)) {
        return invalid_result(EventValidationError::EmptyCorrelationId,
                              "correlation_id", "correlation_id must not be empty when present");
    }

    const bool has_reason_code = event.reason_code.has_value();
    const bool has_reason_text = event.reason_text.has_value();
    if (has_reason_code != has_reason_text) {
        return invalid_result(EventValidationError::PartialReason,
                              "reason_code/reason_text",
                              "reason_code and reason_text must be supplied together");
    }
    if (has_reason_code && !has_non_whitespace(*event.reason_code)) {
        return invalid_result(EventValidationError::EmptyReasonCode,
                              "reason_code", "reason_code must not be empty when present");
    }
    if (has_reason_text && !has_non_whitespace(*event.reason_text)) {
        return invalid_result(EventValidationError::EmptyReasonText,
                              "reason_text", "reason_text must not be empty when present");
    }

    return valid_result();
}

}  // namespace dockstation::core
