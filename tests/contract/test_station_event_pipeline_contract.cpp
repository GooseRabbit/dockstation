#include <cstdlib>
#include <iostream>
#include <string_view>

#include "core/station_event_pipeline.hpp"

using namespace dockstation::core;

namespace {

int checks = 0;
int failures = 0;

void expect(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cout << "FAIL: " << description << '\n';
    }
}

Event make_event(std::string_view id, std::string_view type, std::uint64_t sequence) {
    Event event{};
    event.event_id = id;
    event.event_type = type;
    event.source = "station-events";
    event.source_instance = "boot-1";
    event.received_at = "receiver-time";
    event.source_sequence = sequence;
    event.operation_id = std::nullopt;
    event.correlation_id = std::nullopt;
    event.payload = std::string_view{"opaque-payload"};
    event.quality_status = "VALID";
    event.schema_version = "1";
    return event;
}

SourceCursor make_cursor(std::uint64_t last_sequence) {
    return {"station-events", std::string_view{"boot-1"}, last_sequence};
}

class FakeEventRepository final : public EventRepository {
public:
    EventRepositoryLookupStatus status{EventRepositoryLookupStatus::NotFound};
    Event stored_event{};
    int calls{0};

    EventRepositoryLookupResult find_by_id(std::string_view event_id) noexcept override {
        ++calls;
        if (status == EventRepositoryLookupStatus::Found && stored_event.event_id != event_id) {
            return {EventRepositoryLookupStatus::NotFound, {}};
        }
        if (status == EventRepositoryLookupStatus::Found) {
            return {status, stored_event};
        }
        return {status, {}};
    }
};

class RecordingProcessor final : public EventProcessorPort {
public:
    EventProcessorStatus result{EventProcessorStatus::Processed};
    int calls{0};
    const Event* last_event{nullptr};
    std::string_view last_event_type{};
    EventCategory last_category{EventCategory::Unknown};
    const SourceCursor* last_cursor{nullptr};

    EventProcessorStatus process(const Event& event, EventCategory category,
                                 const SourceCursor& cursor) noexcept override {
        ++calls;
        last_event = &event;
        last_event_type = event.event_type;
        last_category = category;
        last_cursor = &cursor;
        return result;
    }
};

}  // namespace

int main() {
    FakeEventRepository repository;
    RecordingProcessor global_processor;
    RecordingProcessor operation_processor;
    RecordingProcessor timeout_processor;
    operation_processor.result = EventProcessorStatus::NoStateChange;
    timeout_processor.result = EventProcessorStatus::NoStateChange;

    StationEventPipeline pipeline(repository, global_processor,
                                  operation_processor, timeout_processor);
    const EventValidationPolicy source_policy{true, true, false};

    expect(pipeline.is_configured(), "pipeline starts with a valid built-in route table");

    // All five approved event/category pairs pass the real F13 common pipeline.
    const struct RouteCase {
        const char* id;
        const char* type;
        EventHandlingStatus expected_status;
        int expected_global_calls;
        int expected_operation_calls;
        int expected_timeout_calls;
    } cases[] = {
        {"evt-1", "STARTUP_REQUESTED", EventHandlingStatus::Processed, 1, 0, 0},
        {"evt-2", "INITIALIZATION_COMPLETED", EventHandlingStatus::Processed, 2, 0, 0},
        {"evt-3", "SELF_TEST_COMPLETED", EventHandlingStatus::Processed, 3, 0, 0},
        {"evt-4", "OPERATION_STATUS_UPDATED", EventHandlingStatus::NoStateChange, 3, 1, 0},
        {"evt-5", "OPERATION_TIMEOUT", EventHandlingStatus::NoStateChange, 3, 1, 1}
    };

    std::uint64_t sequence = 1;
    for (const RouteCase& route : cases) {
        Event event = make_event(route.id, route.type, sequence);
        const SourceCursor cursor = make_cursor(sequence - 1U);
        const EventHandlingResult result = pipeline.handle(event, source_policy, cursor);

        expect(result.status == route.expected_status,
               "the exact event is accepted through F13 and its selected profile");
        expect(global_processor.calls == route.expected_global_calls &&
                   operation_processor.calls == route.expected_operation_calls &&
                   timeout_processor.calls == route.expected_timeout_calls,
               "exactly one intended profile receives each event");
        ++sequence;
    }

    expect(global_processor.last_event != nullptr &&
               global_processor.last_event_type == "SELF_TEST_COMPLETED" &&
               global_processor.last_category == EventCategory::Subsystem,
           "all three startup events use the one global profile");
    expect(operation_processor.last_event != nullptr &&
               operation_processor.last_event_type == "OPERATION_STATUS_UPDATED" &&
               operation_processor.last_category == EventCategory::Operation,
           "operation status reaches only the operation profile");
    expect(timeout_processor.last_event != nullptr &&
               timeout_processor.last_event_type == "OPERATION_TIMEOUT" &&
               timeout_processor.last_category == EventCategory::Timer,
           "operation timeout reaches only the timer profile");

    // F01 blocks invalid envelopes before repository or profile calls.
    {
        Event event = make_event("invalid", "OPERATION_TIMEOUT", 6);
        event.event_id = "   ";
        const int repository_calls_before = repository.calls;
        const int timeout_calls_before = timeout_processor.calls;
        const EventHandlingResult result = pipeline.handle(event, source_policy, make_cursor(5));
        expect(result.status == EventHandlingStatus::InvalidEvent,
               "invalid envelope is rejected by F01");
        expect(repository.calls == repository_calls_before &&
                   timeout_processor.calls == timeout_calls_before,
               "F01 failure causes no repository/profile access");
    }

    // F02 refuses unknown types before duplicate/freshness lookup.
    {
        Event event = make_event("unknown", "SOMETHING_NEW", 6);
        const int repository_calls_before = repository.calls;
        const EventHandlingResult result = pipeline.handle(event, source_policy, make_cursor(5));
        expect(result.status == EventHandlingStatus::UnknownEventType,
               "unknown event type is rejected by F02");
        expect(repository.calls == repository_calls_before,
               "F02 failure occurs before duplicate lookup");
    }

    // F03 duplicate and repository errors must stop before routing.
    {
        Event event = make_event("duplicate", "OPERATION_TIMEOUT", 6);
        repository.status = EventRepositoryLookupStatus::Found;
        repository.stored_event = event;
        const int timeout_calls_before = timeout_processor.calls;
        const EventHandlingResult result = pipeline.handle(event, source_policy, make_cursor(5));
        expect(result.status == EventHandlingStatus::Duplicate,
               "F03 detects an already stored event");
        expect(timeout_processor.calls == timeout_calls_before,
               "duplicate event is not dispatched to a profile");
    }
    {
        Event event = make_event("repo-error", "OPERATION_TIMEOUT", 6);
        repository.status = EventRepositoryLookupStatus::Error;
        const int timeout_calls_before = timeout_processor.calls;
        const EventHandlingResult result = pipeline.handle(event, source_policy, make_cursor(5));
        expect(result.status == EventHandlingStatus::RepositoryError,
               "F03 repository error fails closed");
        expect(timeout_processor.calls == timeout_calls_before,
               "repository error is not dispatched to a profile");
    }
    repository.status = EventRepositoryLookupStatus::NotFound;

    // F04 stale and unordered events stop before the route table.
    {
        Event event = make_event("stale", "OPERATION_TIMEOUT", 4);
        const int timeout_calls_before = timeout_processor.calls;
        const EventHandlingResult result = pipeline.handle(event, source_policy, make_cursor(5));
        expect(result.status == EventHandlingStatus::StaleEvent,
               "F04 rejects a regressed sequence");
        expect(timeout_processor.calls == timeout_calls_before,
               "stale event is not dispatched");
    }
    {
        Event event = make_event("repeated-sequence", "OPERATION_TIMEOUT", 5);
        const int timeout_calls_before = timeout_processor.calls;
        const EventHandlingResult result = pipeline.handle(event, source_policy, make_cursor(5));
        expect(result.status == EventHandlingStatus::EventOrderUnknown,
               "F04 rejects a repeated sequence as unordered");
        expect(timeout_processor.calls == timeout_calls_before,
               "unordered event is not dispatched");
    }

    // A registered catalogue type without a profile remains explicitly unsupported.
    {
        Event event = make_event("not-yet-supported", "SUBSYSTEM_STATUS_UPDATED", 6);
        const EventHandlingResult result = pipeline.handle(event, source_policy, make_cursor(5));
        expect(result.status == EventHandlingStatus::Unsupported,
               "known but unimplemented event profile remains unsupported");
    }

    // A profile cannot silently decline an event for a route that selected it.
    {
        timeout_processor.result = EventProcessorStatus::Unsupported;
        Event event = make_event("profile-mismatch", "OPERATION_TIMEOUT", 6);
        const EventHandlingResult result = pipeline.handle(event, source_policy, make_cursor(5));
        expect(result.status == EventHandlingStatus::ProcessingError,
               "registered profile returning Unsupported is an integration error");
    }

    std::cout << "Checks: " << checks << '\n';
    std::cout << "Failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
