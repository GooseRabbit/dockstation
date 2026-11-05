#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace dockstation::core {

// Normalized event envelope used by StationStateManager.
// string_view fields are non-owning: their data must remain alive during the
// validation call. The validator does not retain references or allocate memory.
// The payload is opaque here: its serialization and event-specific schema
// are validated by the corresponding adapter/contract, not by this function.
struct Event {
    std::string_view event_id;
    std::string_view event_type;
    std::string_view source;

    // Present when the source can be restarted and needs session-level identity.
    std::optional<std::string_view> source_instance;

    // Source event time is optional; received_at is assigned by the receiver.
    std::optional<std::string_view> occurred_at;
    std::string_view received_at;

    // Optional because not every source supports sequence numbering.
    std::optional<std::uint64_t> source_sequence;

    // Both identifiers are present for events related to a long-running operation
    // or its originating request.
    std::optional<std::string_view> operation_id;
    std::optional<std::string_view> correlation_id;

    // A present but empty payload is allowed; its format is not fixed yet.
    std::optional<std::string_view> payload;

    // Data-quality status from docs/06: VALID, STALE, INVALID, UNAVAILABLE,
    // UNKNOWN or ERROR.
    std::string_view quality_status;

    // Non-empty contract version. Supported-version negotiation is out of scope.
    std::string_view schema_version;

    // Optional pair, populated only when a failure/block reason is identified.
    std::optional<std::string_view> reason_code;
    std::optional<std::string_view> reason_text;
};

// These requirements are supplied by the trusted source/operation contract.
// Event type classification is performed separately by classify_event() using
// the agreed catalogue in docs/06 §2.1; validate_event() does not infer policy
// requirements from event_type.
struct EventValidationPolicy {
    bool require_source_instance{false};
    bool require_source_sequence{false};
    bool require_operation_context{false};
};

}  // namespace dockstation::core
