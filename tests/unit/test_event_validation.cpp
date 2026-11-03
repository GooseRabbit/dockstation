#include <cstdlib>
#include <iostream>
#include <string_view>

#include "core/event_validation.hpp"

using dockstation::core::Event;
using dockstation::core::EventValidationError;
using dockstation::core::EventValidationPolicy;
using dockstation::core::validate_event;

namespace {

Event make_valid_event() {
    Event event{};
    event.event_id = "evt-001";
    event.event_type = "DIAGNOSTIC_COMPLETED";
    event.source = "DiagnosticsManager";
    event.occurred_at = "2026-11-03T10:00:00Z";
    event.received_at = "2026-11-03T10:00:01Z";
    event.payload = "{}";
    event.quality_status = "VALID";
    event.schema_version = "1";
    return event;
}

int failures = 0;

void expect(std::string_view name,
            const dockstation::core::EventValidationResult& result,
            bool expected_valid,
            EventValidationError expected_error = EventValidationError::None) {
    const bool passed = result.valid == expected_valid &&
                        (expected_valid || result.error == expected_error);
    std::cout << (passed ? "PASS" : "FAIL") << " - " << name;
    if (!passed) {
        std::cout << " (valid=" << result.valid
                  << ", error=" << static_cast<int>(result.error)
                  << ", field=" << result.field << ")";
        ++failures;
    }
    std::cout << '\n';
}

}  // namespace

int main() {
    {
        const auto event = make_valid_event();
        expect("valid event", validate_event(event), true);
    }
    {
        auto event = make_valid_event();
        event.event_id = "   ";
        expect("whitespace-only event_id", validate_event(event), false,
               EventValidationError::MissingEventId);
    }
    {
        auto event = make_valid_event();
        event.event_type = {};
        expect("missing event_type", validate_event(event), false,
               EventValidationError::MissingEventType);
    }
    {
        auto event = make_valid_event();
        event.source = {};
        expect("missing source", validate_event(event), false,
               EventValidationError::MissingSource);
    }
    {
        auto event = make_valid_event();
        event.received_at = {};
        expect("missing received_at", validate_event(event), false,
               EventValidationError::MissingReceivedAt);
    }
    {
        auto event = make_valid_event();
        event.payload = std::nullopt;
        expect("missing payload", validate_event(event), false,
               EventValidationError::MissingPayload);
    }
    {
        auto event = make_valid_event();
        event.payload = std::string_view{};
        expect("present but empty payload is accepted by envelope validator",
               validate_event(event), true);
    }
    {
        auto event = make_valid_event();
        event.quality_status = {};
        expect("missing quality_status", validate_event(event), false,
               EventValidationError::MissingQualityStatus);
    }
    {
        constexpr std::string_view statuses[] = {
            "VALID", "STALE", "INVALID", "UNAVAILABLE", "UNKNOWN", "ERROR"};
        bool all_accepted = true;
        for (const auto status : statuses) {
            auto event = make_valid_event();
            event.quality_status = status;
            all_accepted = all_accepted && validate_event(event).valid;
        }
        const dockstation::core::EventValidationResult result{
            all_accepted, EventValidationError::None, {}, {}};
        expect("all documented quality statuses", result, true);
    }
    {
        auto event = make_valid_event();
        event.quality_status = "READY";
        expect("unknown quality_status", validate_event(event), false,
               EventValidationError::UnknownQualityStatus);
    }
    {
        auto event = make_valid_event();
        event.schema_version = {};
        expect("missing schema_version", validate_event(event), false,
               EventValidationError::MissingSchemaVersion);
    }
    {
        auto event = make_valid_event();
        event.occurred_at = std::string_view{"  "};
        expect("empty occurred_at when supplied", validate_event(event), false,
               EventValidationError::EmptyOccurredAt);
    }
    {
        auto event = make_valid_event();
        event.source_instance = std::string_view{"  "};
        expect("empty source_instance when supplied", validate_event(event), false,
               EventValidationError::EmptySourceInstance);
    }
    {
        auto event = make_valid_event();
        EventValidationPolicy policy{};
        policy.require_source_instance = true;
        expect("source_instance required by policy", validate_event(event, policy), false,
               EventValidationError::MissingSourceInstance);
    }
    {
        auto event = make_valid_event();
        event.source_instance = "boot-7";
        EventValidationPolicy policy{};
        policy.require_source_instance = true;
        expect("source_instance supplied", validate_event(event, policy), true);
    }
    {
        auto event = make_valid_event();
        EventValidationPolicy policy{};
        policy.require_source_sequence = true;
        expect("source_sequence required by policy", validate_event(event, policy), false,
               EventValidationError::MissingSourceSequence);
    }
    {
        auto event = make_valid_event();
        event.source_sequence = 0;
        EventValidationPolicy policy{};
        policy.require_source_sequence = true;
        expect("zero is accepted as a sequence value", validate_event(event, policy), true);
    }
    {
        auto event = make_valid_event();
        event.operation_id = "op-1";
        EventValidationPolicy policy{};
        expect("partial operation context", validate_event(event, policy), false,
               EventValidationError::PartialOperationContext);
    }
    {
        auto event = make_valid_event();
        EventValidationPolicy policy{};
        policy.require_operation_context = true;
        expect("operation context required by policy", validate_event(event, policy), false,
               EventValidationError::MissingOperationContext);
    }
    {
        auto event = make_valid_event();
        event.operation_id = "op-1";
        event.correlation_id = "corr-1";
        EventValidationPolicy policy{};
        policy.require_operation_context = true;
        expect("complete operation context", validate_event(event, policy), true);
    }
    {
        auto event = make_valid_event();
        event.operation_id = "   ";
        event.correlation_id = "corr-1";
        expect("empty operation_id", validate_event(event), false,
               EventValidationError::EmptyOperationId);
    }
    {
        auto event = make_valid_event();
        event.operation_id = "op-1";
        event.correlation_id = "   ";
        expect("empty correlation_id", validate_event(event), false,
               EventValidationError::EmptyCorrelationId);
    }
    {
        auto event = make_valid_event();
        event.reason_code = "MOTOR_TIMEOUT";
        expect("partial reason pair", validate_event(event), false,
               EventValidationError::PartialReason);
    }
    {
        auto event = make_valid_event();
        event.reason_code = "MOTOR_TIMEOUT";
        event.reason_text = "Motor did not reach target position";
        expect("complete reason pair", validate_event(event), true);
    }
    {
        auto event = make_valid_event();
        event.reason_code = "  ";
        event.reason_text = "Some cause";
        expect("empty reason_code", validate_event(event), false,
               EventValidationError::EmptyReasonCode);
    }
    {
        auto event = make_valid_event();
        event.reason_code = "MOTOR_TIMEOUT";
        event.reason_text = "  ";
        expect("empty reason text", validate_event(event), false,
               EventValidationError::EmptyReasonText);
    }

    std::cout << "\nTests failed: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
