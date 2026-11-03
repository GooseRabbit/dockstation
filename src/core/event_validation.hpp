#pragma once

#include <string_view>

#include "event.hpp"

namespace dockstation::core {

enum class EventValidationError {
    None = 0,
    MissingEventId,
    MissingEventType,
    MissingSource,
    MissingReceivedAt,
    MissingPayload,
    MissingQualityStatus,
    UnknownQualityStatus,
    MissingSchemaVersion,
    EmptyOccurredAt,
    EmptySourceInstance,
    MissingSourceInstance,
    MissingSourceSequence,
    MissingOperationContext,
    EmptyOperationId,
    EmptyCorrelationId,
    PartialOperationContext,
    EmptyReasonCode,
    EmptyReasonText,
    PartialReason,
};

struct EventValidationResult {
    bool valid{false};
    EventValidationError error{EventValidationError::None};
    std::string_view field{};
    std::string_view message{};

    [[nodiscard]] explicit operator bool() const noexcept { return valid; }
};

// Pure validation of the common event envelope.
// Does not inspect payload contents, check event_id uniqueness/freshness,
// parse timestamp format, or perform I/O. Those responsibilities belong to
// event-specific adapters and later SSM functions (for example F03/F04).
[[nodiscard]] EventValidationResult validate_event(
    const Event& event,
    const EventValidationPolicy& policy = {}) noexcept;

}  // namespace dockstation::core
