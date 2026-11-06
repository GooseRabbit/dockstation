#include "core/event_handling.hpp"
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

using namespace dockstation::core;

namespace {
int checks = 0;

void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakeEventRepository final : public EventRepository {
public:
    EventRepositoryLookupResult find_by_id(std::string_view id) noexcept override {
        ++calls;
        if (status == EventRepositoryLookupStatus::Found && stored.event_id.empty()) {
            stored.event_id = id;
        }
        return {status, stored};
    }
    EventRepositoryLookupStatus status{EventRepositoryLookupStatus::NotFound};
    Event stored{};
    int calls{0};
};

class FakeProcessor final : public EventProcessorPort {
public:
    EventProcessorStatus process(const Event& event, EventCategory category,
                                 const SourceCursor& cursor) noexcept override {
        ++calls;
        seen_event = &event;
        seen_category = category;
        seen_cursor = &cursor;
        return next_status;
    }
    int calls{0};
    const Event* seen_event{nullptr};
    const SourceCursor* seen_cursor{nullptr};
    EventCategory seen_category{EventCategory::Unknown};
    EventProcessorStatus next_status{EventProcessorStatus::Processed};
};

Event make_event() {
    return {"event-1", "STARTUP_REQUESTED", "timer-1", std::string_view{"boot-1"},
            std::string_view{"2026-11-06T08:00:00"}, "2026-11-06T08:00:01", 7,
            std::nullopt, std::nullopt, std::string_view{"{}"}, "VALID", "1.0",
            std::nullopt, std::nullopt};
}

void reset(FakeEventRepository& repo, FakeProcessor& processor) {
    repo.status = EventRepositoryLookupStatus::NotFound;
    repo.stored = {};
    repo.calls = 0;
    processor.calls = 0;
    processor.seen_event = nullptr;
    processor.seen_cursor = nullptr;
    processor.seen_category = EventCategory::Unknown;
    processor.next_status = EventProcessorStatus::Processed;
}
}  // namespace

int main() {
    FakeEventRepository repo;
    FakeProcessor processor;
    Event event = make_event();
    EventValidationPolicy policy{};
    SourceCursor cursor{"timer-1", std::string_view{"boot-1"}, 6};
    EventHandlingResult result{};

    // Invalid input stops before event lookup or profile processing.
    Event invalid = event;
    invalid.event_id = "  ";
    reset(repo, processor);
    result = handle_event(invalid, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::InvalidEvent &&
           result.validation_error == EventValidationError::MissingEventId,
           "invalid envelope rejected with diagnostic");
    expect(repo.calls == 0 && processor.calls == 0,
           "invalid envelope has no downstream calls");

    // Unknown type is not sent to the repository or processor.
    Event unknown = event;
    unknown.event_type = "STARTUP_REQUEST";
    reset(repo, processor);
    result = handle_event(unknown, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::UnknownEventType,
           "unknown event type rejected exactly");
    expect(repo.calls == 0 && processor.calls == 0,
           "unknown type stops pipeline");

    // Exact duplicate ignores receiver-assigned received_at; changed payload is a conflict.
    reset(repo, processor);
    repo.status = EventRepositoryLookupStatus::Found;
    repo.stored = event;
    repo.stored.received_at = "different-receiver-time";
    result = handle_event(event, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::Duplicate && processor.calls == 0,
           "exact duplicate blocked");

    reset(repo, processor);
    repo.status = EventRepositoryLookupStatus::Found;
    repo.stored = event;
    repo.stored.payload = "{different}";
    result = handle_event(event, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::EventConflict && processor.calls == 0,
           "same ID with changed content blocked");

    reset(repo, processor);
    repo.status = EventRepositoryLookupStatus::Error;
    result = handle_event(event, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::RepositoryError && processor.calls == 0,
           "repository error fails closed");

    // Freshness gates: old, repeated, missing-sequence, and wrong-source events are not handled.
    reset(repo, processor);
    Event stale = event;
    stale.source_sequence = 5;
    result = handle_event(stale, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::StaleEvent &&
           result.freshness_reason == EventFreshnessReason::SequenceRegressed,
           "regressed sequence rejected");
    expect(processor.calls == 0, "stale event not delegated");

    reset(repo, processor);
    Event repeated = event;
    repeated.source_sequence = 6;
    result = handle_event(repeated, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::EventOrderUnknown &&
           result.freshness_reason == EventFreshnessReason::SequenceRepeated,
           "repeated sequence is unordered");

    reset(repo, processor);
    Event no_sequence = event;
    no_sequence.source_sequence.reset();
    result = handle_event(no_sequence, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::EventOrderUnknown &&
           result.freshness_reason == EventFreshnessReason::SequenceMissing,
           "missing sequence is unordered");

    reset(repo, processor);
    SourceCursor wrong_source = cursor;
    wrong_source.source = "other-source";
    result = handle_event(event, policy, wrong_source, repo, processor);
    expect(result.status == EventHandlingStatus::EventOrderUnknown &&
           result.freshness_reason == EventFreshnessReason::SourceMismatch,
           "wrong source cursor is unordered");

    reset(repo, processor);
    SourceCursor wrong_session = cursor;
    wrong_session.source_instance = "old-boot";
    result = handle_event(event, policy, wrong_session, repo, processor);
    expect(result.status == EventHandlingStatus::EventOrderUnknown &&
           result.freshness_reason == EventFreshnessReason::SourceInstanceMismatch,
           "wrong source session is unordered");

    // Happy path passes the original event, category and unmodified cursor to the profile processor.
    reset(repo, processor);
    const Event original = event;
    const SourceCursor original_cursor = cursor;
    result = handle_event(event, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::Processed &&
           result.category == EventCategory::Command,
           "valid current event processed");
    expect(processor.calls == 1 && processor.seen_event == &event &&
           processor.seen_cursor == &cursor,
           "same event and cursor forwarded once");
    expect(processor.seen_category == EventCategory::Command,
           "correct category forwarded");
    expect(event.event_id == original.event_id &&
           cursor.last_sequence == original_cursor.last_sequence,
           "inputs remain unchanged");

    // A structurally valid subsystem event with UNKNOWN data quality is routed to its profile handler.
    // That handler, not the generic F13 gate, decides whether the payload may change subsystem status.
    reset(repo, processor);
    Event uncertain = event;
    uncertain.event_type = "SUBSYSTEM_STATUS_UPDATED";
    uncertain.quality_status = "UNKNOWN";
    result = handle_event(uncertain, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::Processed && processor.calls == 1,
           "unknown data quality is delegated for event-specific handling");
    expect(processor.seen_event->quality_status == "UNKNOWN",
           "original data-quality status remains unchanged");

    // Each registered category reaches the processor without guessing from payload text.
    const std::pair<std::string_view, EventCategory> categories[] = {
        {"SELF_TEST_COMPLETED", EventCategory::Subsystem},
        {"OPERATION_STATUS_UPDATED", EventCategory::Operation},
        {"SAFETY_PERMISSION_UPDATED", EventCategory::Safety},
        {"COMMAND_REQUESTED", EventCategory::Command},
        {"OPERATION_TIMEOUT", EventCategory::Timer},
        {"RECOVERY_REQUESTED", EventCategory::Recovery}
    };
    for (const auto& category_case : categories) {
        reset(repo, processor);
        Event categorized = event;
        categorized.event_type = category_case.first;
        result = handle_event(categorized, policy, cursor, repo, processor);
        expect(result.status == EventHandlingStatus::Processed,
               "registered category accepted for dispatch");
        expect(processor.seen_category == category_case.second,
               "registered category forwarded exactly");
    }

    // A policy can require operation context; F13 applies it before classifying or looking up the event.
    reset(repo, processor);
    EventValidationPolicy operation_policy{};
    operation_policy.require_operation_context = true;
    result = handle_event(event, operation_policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::InvalidEvent &&
           result.validation_error == EventValidationError::MissingOperationContext,
           "source policy applied");
    expect(repo.calls == 0 && processor.calls == 0,
           "policy failure has no downstream calls");

    const std::pair<EventProcessorStatus, EventHandlingStatus> mappings[] = {
        {EventProcessorStatus::NoStateChange, EventHandlingStatus::NoStateChange},
        {EventProcessorStatus::Rejected, EventHandlingStatus::Rejected},
        {EventProcessorStatus::Unsupported, EventHandlingStatus::Unsupported},
        {EventProcessorStatus::Duplicate, EventHandlingStatus::Duplicate},
        {EventProcessorStatus::EventConflict, EventHandlingStatus::EventConflict},
        {EventProcessorStatus::ConcurrentUpdate, EventHandlingStatus::ConcurrentUpdate},
        {EventProcessorStatus::Error, EventHandlingStatus::ProcessingError}
    };
    for (const auto& mapping : mappings) {
        reset(repo, processor);
        processor.next_status = mapping.first;
        result = handle_event(event, policy, cursor, repo, processor);
        expect(result.status == mapping.second,
               "profile processor status mapped without false success");
        expect(processor.calls == 1, "profile processor invoked exactly once");
    }

    reset(repo, processor);
    processor.next_status = static_cast<EventProcessorStatus>(255);
    result = handle_event(event, policy, cursor, repo, processor);
    expect(result.status == EventHandlingStatus::ProcessingError,
           "unknown processor status fails closed");

    std::cout << "PASS: " << checks << " assertions\n";
    return 0;
}
