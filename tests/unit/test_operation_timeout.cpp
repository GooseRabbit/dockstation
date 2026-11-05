#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

#include "core/operations.hpp"

using namespace dockstation::core;

namespace {
int failures = 0;
int checks = 0;

void expect(std::string_view name, bool passed) {
    ++checks;
    std::cout << (passed ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!passed) ++failures;
}

Event make_timeout_event(std::string_view event_id = "timer-event-1",
                          std::uint64_t sequence = 5) {
    Event event{};
    event.event_id = event_id;
    event.event_type = "OPERATION_TIMEOUT";
    event.source = "timer-service";
    event.source_instance = "timer-boot-1";
    event.received_at = "2026-11-05T15:00:00Z";
    event.source_sequence = sequence;
    event.operation_id = "operation-1";
    event.correlation_id = "correlation-1";
    event.payload = "typed timeout details";
    event.quality_status = "VALID";
    event.schema_version = "1.0";
    return event;
}

SourceCursor make_cursor(std::uint64_t last_sequence = 4) {
    return {"timer-service", std::string_view{"timer-boot-1"}, last_sequence};
}

OperationRecord make_record(
    OperationLifecycleStatus status = OperationLifecycleStatus::Requested,
    std::uint64_t revision = 1) {
    const ActionRequest request{
        "operation-1", "correlation-1", "lid-manager", "open-lid", std::nullopt};
    return {request, status, revision};
}

struct FakeRepository final : OperationUpdateRepository {
    OperationRecord record{make_record()};
    OperationLookupStatus lookup_status{OperationLookupStatus::Found};
    OperationUpdateCommitStatus commit_status{OperationUpdateCommitStatus::Applied};
    bool has_stored_event{false};
    Event stored_event{};
    int find_event_calls{0};
    int find_operation_calls{0};
    int commit_calls{0};
    OperationLifecycleStatus committed_status{OperationLifecycleStatus::Requested};
    Event committed_event{};
    SourceCursor committed_cursor{};

    EventRepositoryLookupResult find_by_id(std::string_view event_id) noexcept override {
        ++find_event_calls;
        if (!has_stored_event) return {EventRepositoryLookupStatus::NotFound, {}};
        if (stored_event.event_id != event_id) return {EventRepositoryLookupStatus::NotFound, {}};
        return {EventRepositoryLookupStatus::Found, stored_event};
    }

    OperationLookupResult find_operation(std::string_view operation_id) noexcept override {
        ++find_operation_calls;
        if (lookup_status != OperationLookupStatus::Found) {
            return {lookup_status, {}};
        }
        if (record.request.operation_id != operation_id) {
            return {OperationLookupStatus::NotFound, {}};
        }
        return {OperationLookupStatus::Found, record};
    }

    OperationUpdateCommitStatus commit_operation_update(
        const OperationRecord& expected_record,
        OperationLifecycleStatus next_status,
        const Event& event,
        const SourceCursor& expected_cursor) noexcept override {
        ++commit_calls;
        committed_status = next_status;
        committed_event = event;
        committed_cursor = expected_cursor;
        if (commit_status == OperationUpdateCommitStatus::Applied) {
            record.status = next_status;
            record.revision = expected_record.revision + 1U;
            stored_event = event;
            has_stored_event = true;
        }
        return commit_status;
    }
};

OperationTimeoutResult run_timeout(
    FakeRepository& repository,
    OperationTimeoutType type,
    DispatchCertainty certainty,
    Event event = make_timeout_event(),
    SourceCursor cursor = make_cursor()) {
    const OperationTimeout timeout{event, type, certainty};
    return handle_operation_timeout(timeout, cursor, repository);
}
}  // namespace

int main() {
    {
        FakeRepository repository;
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched);
        expect("before-dispatch timeout becomes FAILED",
               result.status == OperationTimeoutResultStatus::Applied &&
               result.resulting_status == OperationLifecycleStatus::Failed);
        expect("before-dispatch timeout increments revision",
               result.revision == 2 && repository.record.revision == 2);
        expect("before-dispatch timeout commits event once",
               repository.commit_calls == 1 &&
               repository.committed_event.event_type == "OPERATION_TIMEOUT");
    }
    {
        FakeRepository repository;
        const auto result = run_timeout(repository, OperationTimeoutType::DispatchResponse,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("dispatch response timeout becomes OUTCOME_UNKNOWN",
               result.status == OperationTimeoutResultStatus::Applied &&
               result.resulting_status == OperationLifecycleStatus::OutcomeUnknown);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::Started, 7);
        const auto result = run_timeout(repository, OperationTimeoutType::OperationCompletion,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("execution timeout after STARTED becomes OUTCOME_UNKNOWN",
               result.status == OperationTimeoutResultStatus::Applied &&
               result.resulting_status == OperationLifecycleStatus::OutcomeUnknown &&
               result.revision == 8);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::CancelRequested, 3);
        const auto result = run_timeout(repository, OperationTimeoutType::CancellationCompletion,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("cancellation confirmation timeout becomes OUTCOME_UNKNOWN",
               result.status == OperationTimeoutResultStatus::Applied &&
               result.resulting_status == OperationLifecycleStatus::OutcomeUnknown);
    }
    {
        FakeRepository repository;
        Event event = make_timeout_event();
        event.event_type = "OPERATION_STATUS_UPDATED";
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched, event);
        expect("wrong event type is rejected", result.status == OperationTimeoutResultStatus::WrongEventType);
        expect("wrong event type causes no lookup or commit",
               repository.find_operation_calls == 0 && repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        Event event = make_timeout_event();
        event.event_id = "";
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched, event);
        expect("invalid event envelope is rejected", result.status == OperationTimeoutResultStatus::InvalidEvent);
        expect("invalid event does not access repository", repository.find_event_calls == 0);
    }
    {
        FakeRepository repository;
        Event event = make_timeout_event();
        event.quality_status = "UNKNOWN";
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched, event);
        expect("unreliable timeout event is rejected",
               result.status == OperationTimeoutResultStatus::EventNotReliable);
        expect("unreliable event is not committed", repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        Event event = make_timeout_event();
        event.correlation_id.reset();
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched, event);
        expect("missing operation correlation is rejected",
               result.status == OperationTimeoutResultStatus::InvalidEvent);
    }
    {
        FakeRepository repository;
        Event event = make_timeout_event();
        event.source_sequence.reset();
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched, event);
        expect("missing timer sequence is unordered",
               result.status == OperationTimeoutResultStatus::EventOrderUnknown);
    }
    {
        FakeRepository repository;
        Event event = make_timeout_event("timer-event-stale", 3);
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched, event);
        expect("regressed timer sequence is stale",
               result.status == OperationTimeoutResultStatus::StaleEvent);
    }
    {
        FakeRepository repository;
        SourceCursor cursor{"other-source", std::string_view{"timer-boot-1"}, 4};
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched,
                                        make_timeout_event(), cursor);
        expect("different timer source is unordered",
               result.status == OperationTimeoutResultStatus::EventOrderUnknown);
    }
    {
        FakeRepository repository;
        SourceCursor cursor{"timer-service", std::string_view{"previous-boot"}, 4};
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched,
                                        make_timeout_event(), cursor);
        expect("different timer session is unordered",
               result.status == OperationTimeoutResultStatus::EventOrderUnknown);
    }
    {
        FakeRepository repository;
        const Event event = make_timeout_event();
        repository.has_stored_event = true;
        repository.stored_event = event;
        repository.stored_event.received_at = "different receiver timestamp";
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched, event);
        expect("exact duplicate timeout is ignored",
               result.status == OperationTimeoutResultStatus::Duplicate);
        expect("duplicate timeout does not read or commit operation",
               repository.find_operation_calls == 0 && repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        repository.has_stored_event = true;
        repository.stored_event = make_timeout_event();
        repository.stored_event.payload = "different payload";
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched);
        expect("event ID conflict is rejected",
               result.status == OperationTimeoutResultStatus::EventConflict);
        expect("event conflict does not commit",
               repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        repository.lookup_status = OperationLookupStatus::NotFound;
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched);
        expect("unknown operation is not created",
               result.status == OperationTimeoutResultStatus::OperationNotFound);
        expect("unknown operation does not commit", repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        repository.lookup_status = OperationLookupStatus::Error;
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched);
        expect("operation repository read error is returned",
               result.status == OperationTimeoutResultStatus::RepositoryError);
    }
    {
        FakeRepository repository;
        repository.record.request.correlation_id = "different-correlation";
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched);
        expect("correlation mismatch is rejected",
               result.status == OperationTimeoutResultStatus::CorrelationMismatch);
        expect("correlation mismatch does not commit", repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("contradictory pre-dispatch certainty is rejected",
               result.status == OperationTimeoutResultStatus::InvalidDispatchCertainty);
        expect("contradictory certainty does not access repository",
               repository.find_event_calls == 0 && repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        const auto result = run_timeout(repository, OperationTimeoutType::DispatchResponse,
                                        DispatchCertainty::NotDispatched);
        expect("post-dispatch timeout requires possible-delivery certainty",
               result.status == OperationTimeoutResultStatus::InvalidDispatchCertainty);
    }
    {
        FakeRepository repository;
        OperationTimeout timeout{};
        timeout.event = make_timeout_event();
        const auto result = handle_operation_timeout(timeout, make_cursor(), repository);
        expect("default timeout type is UNKNOWN and fails closed",
               result.status == OperationTimeoutResultStatus::InvalidTimeoutType);
        expect("unclassified timeout does not access repository",
               repository.find_event_calls == 0 && repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::Unknown);
        expect("default dispatch certainty fails closed",
               result.status == OperationTimeoutResultStatus::InvalidDispatchCertainty);
        expect("unknown dispatch certainty does not access repository",
               repository.find_event_calls == 0 && repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        const auto result = run_timeout(repository, static_cast<OperationTimeoutType>(255),
                                        DispatchCertainty::NotDispatched);
        expect("unknown timeout type fails closed",
               result.status == OperationTimeoutResultStatus::InvalidTimeoutType);
    }
    {
        FakeRepository repository;
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        static_cast<DispatchCertainty>(255));
        expect("unknown dispatch certainty fails closed",
               result.status == OperationTimeoutResultStatus::InvalidDispatchCertainty);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::Completed, 4);
        const auto result = run_timeout(repository, OperationTimeoutType::OperationCompletion,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("late timeout cannot reopen COMPLETED",
               result.status == OperationTimeoutResultStatus::LateTimeoutIgnored &&
               repository.record.status == OperationLifecycleStatus::Completed);
        expect("late timeout after terminal state is not committed", repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::Failed, 4);
        const auto result = run_timeout(repository, OperationTimeoutType::OperationCompletion,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("late timeout cannot reopen FAILED",
               result.status == OperationTimeoutResultStatus::LateTimeoutIgnored);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::Cancelled, 4);
        const auto result = run_timeout(repository, OperationTimeoutType::CancellationCompletion,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("late timeout cannot reopen CANCELLED",
               result.status == OperationTimeoutResultStatus::LateTimeoutIgnored);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::OutcomeUnknown, 4);
        const auto result = run_timeout(repository, OperationTimeoutType::OperationCompletion,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("repeated timeout leaves OUTCOME_UNKNOWN unchanged",
               result.status == OperationTimeoutResultStatus::AlreadyOutcomeUnknown &&
               repository.commit_calls == 0);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::Accepted, 4);
        const auto result = run_timeout(repository, OperationTimeoutType::DispatchResponse,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("late dispatch-response timeout does not rewind ACCEPTED",
               result.status == OperationTimeoutResultStatus::LateTimeoutIgnored);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::CancelRequested, 4);
        const auto result = run_timeout(repository, OperationTimeoutType::OperationCompletion,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("operation completion timeout is not a cancellation timeout",
               result.status == OperationTimeoutResultStatus::TimeoutNotApplicable);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::Started, 4);
        const auto result = run_timeout(repository, OperationTimeoutType::CancellationCompletion,
                                        DispatchCertainty::MayHaveBeenDispatched);
        expect("cancellation timeout requires CANCEL_REQUESTED",
               result.status == OperationTimeoutResultStatus::TimeoutNotApplicable);
    }
    {
        FakeRepository repository;
        repository.record = make_record(OperationLifecycleStatus::Requested,
                                        std::numeric_limits<std::uint64_t>::max());
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched);
        expect("revision overflow fails closed",
               result.status == OperationTimeoutResultStatus::RepositoryError);
        expect("revision overflow does not commit", repository.commit_calls == 0);
    }

    const OperationUpdateCommitStatus commit_statuses[] = {
        OperationUpdateCommitStatus::DuplicateEvent,
        OperationUpdateCommitStatus::EventConflict,
        OperationUpdateCommitStatus::OperationChanged,
        OperationUpdateCommitStatus::SourceCursorChanged,
        OperationUpdateCommitStatus::OperationNotFound,
        OperationUpdateCommitStatus::Error,
        static_cast<OperationUpdateCommitStatus>(255)
    };
    const OperationTimeoutResultStatus expected_statuses[] = {
        OperationTimeoutResultStatus::Duplicate,
        OperationTimeoutResultStatus::EventConflict,
        OperationTimeoutResultStatus::ConcurrentOperationUpdate,
        OperationTimeoutResultStatus::ConcurrentSourceUpdate,
        OperationTimeoutResultStatus::OperationNotFound,
        OperationTimeoutResultStatus::RepositoryError,
        OperationTimeoutResultStatus::RepositoryError
    };
    for (std::size_t i = 0; i < sizeof(commit_statuses) / sizeof(commit_statuses[0]); ++i) {
        FakeRepository repository;
        repository.commit_status = commit_statuses[i];
        const auto result = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched);
        expect("repository commit status maps safely",
               result.status == expected_statuses[i] && repository.commit_calls == 1);
    }

    {
        FakeRepository repository;
        const auto first = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                       DispatchCertainty::NotDispatched);
        const auto second = run_timeout(repository, OperationTimeoutType::BeforeDispatch,
                                        DispatchCertainty::NotDispatched);
        expect("repeated delivery after successful commit is idempotent",
               first.status == OperationTimeoutResultStatus::Applied &&
               second.status == OperationTimeoutResultStatus::Duplicate &&
               repository.commit_calls == 1);
    }

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
