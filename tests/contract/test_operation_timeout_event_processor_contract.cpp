#include <cstdlib>
#include <iostream>
#include <string_view>

#include "core/operation_timeout_event_processor.hpp"

using namespace dockstation::core;

namespace {

int checks = 0;
int failures = 0;
int f11_calls = 0;

OperationTimeoutResult f11_result{};
OperationTimeout last_timeout{};
SourceCursor last_cursor{};
OperationUpdateRepository* last_repository = nullptr;

void expect(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cout << "FAIL: " << description << '\\n';
    }
}

Event make_event() {
    Event value{};
    value.event_id = "timer-1";
    value.event_type = "OPERATION_TIMEOUT";
    value.source = "timer";
    value.source_instance = "boot";
    value.received_at = "now";
    value.source_sequence = 5;
    value.operation_id = "op";
    value.correlation_id = "corr";
    value.payload = "opaque";
    value.quality_status = "VALID";
    value.schema_version = "1";
    return value;
}

SourceCursor make_cursor() {
    return {"timer", std::string_view{"boot"}, 4};
}

class FakeDecoder final : public OperationTimeoutDecoderPort {
public:
    OperationTimeoutDecodeResult result{
        OperationTimeoutDecodeStatus::Decoded,
        OperationTimeoutType::BeforeDispatch,
        DispatchCertainty::NotDispatched
    };
    int calls = 0;
    const Event* seen_event = nullptr;

    OperationTimeoutDecodeResult decode_timeout(const Event& event) noexcept override {
        ++calls;
        seen_event = &event;
        return result;
    }
};

class FakeRepository final : public OperationUpdateRepository {
public:
    EventRepositoryLookupResult find_by_id(std::string_view) noexcept override {
        return {EventRepositoryLookupStatus::NotFound, {}};
    }

    OperationLookupResult find_operation(std::string_view) noexcept override {
        return {OperationLookupStatus::NotFound, {}};
    }

    OperationUpdateCommitStatus commit_operation_update(
        const OperationRecord&,
        OperationLifecycleStatus,
        const Event&,
        const SourceCursor&) noexcept override {
        return OperationUpdateCommitStatus::Error;
    }
};

EventProcessorStatus run_processor(
    Event& event,
    FakeDecoder& decoder,
    FakeRepository& repository,
    EventCategory category = EventCategory::Timer,
    SourceCursor cursor = make_cursor()) {
    OperationTimeoutEventProcessor processor(decoder, repository);
    return processor.process(event, category, cursor);
}

}  // namespace

namespace dockstation::core {

// Stub F11 isolates this contract test to the F13 profile adapter.
OperationTimeoutResult handle_operation_timeout(
    const OperationTimeout& timeout,
    const SourceCursor& cursor,
    OperationUpdateRepository& repository) noexcept {
    ++f11_calls;
    last_timeout = timeout;
    last_cursor = cursor;
    last_repository = &repository;
    return f11_result;
}

}  // namespace dockstation::core

int main() {
    // Successful decode forwards the original event and cursor to F11 exactly once.
    {
        Event event = make_event();
        FakeDecoder decoder;
        FakeRepository repository;
        f11_calls = 0;
        f11_result = {
            OperationTimeoutResultStatus::Applied,
            OperationLifecycleStatus::Failed,
            2
        };
        const SourceCursor cursor = make_cursor();

        expect(run_processor(event, decoder, repository, EventCategory::Timer, cursor) ==
                   EventProcessorStatus::NoStateChange,
               "Applied maps to NoStateChange");
        expect(decoder.calls == 1 && f11_calls == 1,
               "valid event is decoded and passed to F11 once");
        expect(decoder.seen_event == &event && last_timeout.event.payload == event.payload,
               "original event is forwarded unchanged");
        expect(last_cursor.source == cursor.source &&
                   last_cursor.source_instance == cursor.source_instance &&
                   last_cursor.last_sequence == cursor.last_sequence,
               "source cursor is forwarded unchanged");
        expect(last_repository == &repository &&
                   last_timeout.timeout_type == OperationTimeoutType::BeforeDispatch &&
                   last_timeout.dispatch_certainty == DispatchCertainty::NotDispatched,
               "typed timeout evidence and repository are forwarded");
    }

    // Only the registered event/category pair is handled by this profile.
    {
        Event event = make_event();
        FakeDecoder decoder;
        FakeRepository repository;
        f11_calls = 0;

        expect(run_processor(event, decoder, repository, EventCategory::Operation) ==
                   EventProcessorStatus::Unsupported,
               "wrong category is unsupported");

        event = make_event();
        event.event_type = "OPERATION_STATUS_UPDATED";
        expect(run_processor(event, decoder, repository) == EventProcessorStatus::Unsupported,
               "wrong event type is unsupported");
        expect(decoder.calls == 0 && f11_calls == 0,
               "unsupported event has no downstream effects");
    }

    // Data quality is checked before the payload decoder or F11 can be called.
    for (const std::string_view quality : {
             "STALE", "INVALID", "UNAVAILABLE", "UNKNOWN", "ERROR", "valid", ""
         }) {
        Event event = make_event();
        event.quality_status = quality;
        FakeDecoder decoder;
        FakeRepository repository;
        f11_calls = 0;

        expect(run_processor(event, decoder, repository) == EventProcessorStatus::Rejected,
               "non-VALID quality is rejected");
        expect(decoder.calls == 0 && f11_calls == 0,
               "quality gate blocks decoder and F11");
    }

    // Decoder outcomes and missing typed evidence fail closed.
    {
        Event event = make_event();
        FakeDecoder decoder;
        FakeRepository repository;
        decoder.result.status = OperationTimeoutDecodeStatus::UnsupportedPayload;
        f11_calls = 0;
        expect(run_processor(event, decoder, repository) == EventProcessorStatus::Rejected &&
                   f11_calls == 0,
               "unsupported payload is rejected");
    }
    {
        Event event = make_event();
        FakeDecoder decoder;
        FakeRepository repository;
        decoder.result.status = OperationTimeoutDecodeStatus::Error;
        f11_calls = 0;
        expect(run_processor(event, decoder, repository) == EventProcessorStatus::Error &&
                   f11_calls == 0,
               "decoder error fails closed");
    }
    {
        Event event = make_event();
        FakeDecoder decoder;
        FakeRepository repository;
        decoder.result.status = static_cast<OperationTimeoutDecodeStatus>(255);
        f11_calls = 0;
        expect(run_processor(event, decoder, repository) == EventProcessorStatus::Error &&
                   f11_calls == 0,
               "unknown decoder enum fails closed");
    }
    {
        Event event = make_event();
        FakeDecoder decoder;
        FakeRepository repository;
        decoder.result.timeout_type = OperationTimeoutType::Unknown;
        f11_calls = 0;
        expect(run_processor(event, decoder, repository) == EventProcessorStatus::Rejected &&
                   f11_calls == 0,
               "UNKNOWN timeout type is rejected");
    }
    {
        Event event = make_event();
        FakeDecoder decoder;
        FakeRepository repository;
        decoder.result.dispatch_certainty = DispatchCertainty::Unknown;
        f11_calls = 0;
        expect(run_processor(event, decoder, repository) == EventProcessorStatus::Rejected &&
                   f11_calls == 0,
               "UNKNOWN dispatch certainty is rejected");
    }

    // Every known F11 outcome and an unknown numeric value have an explicit F13 mapping.
    struct ResultMapping {
        OperationTimeoutResultStatus f11_status;
        EventProcessorStatus f13_status;
    };

    const ResultMapping mappings[] = {
        {OperationTimeoutResultStatus::Applied, EventProcessorStatus::NoStateChange},
        {OperationTimeoutResultStatus::Duplicate, EventProcessorStatus::Duplicate},
        {OperationTimeoutResultStatus::EventConflict, EventProcessorStatus::EventConflict},
        {OperationTimeoutResultStatus::InvalidEvent, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::EventNotReliable, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::WrongEventType, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::InvalidTimeoutType, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::InvalidDispatchCertainty, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::StaleEvent, EventProcessorStatus::Error},
        {OperationTimeoutResultStatus::EventOrderUnknown, EventProcessorStatus::Error},
        {OperationTimeoutResultStatus::OperationNotFound, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::CorrelationMismatch, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::TimeoutNotApplicable, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::AlreadyOutcomeUnknown, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::LateTimeoutIgnored, EventProcessorStatus::Rejected},
        {OperationTimeoutResultStatus::ConcurrentOperationUpdate, EventProcessorStatus::ConcurrentUpdate},
        {OperationTimeoutResultStatus::ConcurrentSourceUpdate, EventProcessorStatus::ConcurrentUpdate},
        {OperationTimeoutResultStatus::RepositoryError, EventProcessorStatus::Error},
        {static_cast<OperationTimeoutResultStatus>(255), EventProcessorStatus::Error}
    };

    for (const ResultMapping& mapping : mappings) {
        Event event = make_event();
        FakeDecoder decoder;
        FakeRepository repository;
        f11_calls = 0;
        f11_result = {
            mapping.f11_status,
            OperationLifecycleStatus::Failed,
            2
        };

        expect(run_processor(event, decoder, repository) == mapping.f13_status,
               "F11 result maps to the expected F13 status");
        expect(f11_calls == 1,
               "F11 is called once after valid decoding");
    }

    std::cout << "Checks: " << checks << '\\n';
    std::cout << "Failures: " << failures << '\\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
