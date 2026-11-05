#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

#include "core/operations.hpp"

using namespace dockstation::core;

namespace {
int failures = 0;

bool same_optional(const std::optional<std::string_view>& left,
                   const std::optional<std::string_view>& right) noexcept {
    return left.has_value() == right.has_value() &&
           (!left.has_value() || *left == *right);
}

bool same_event(const Event& left, const Event& right) noexcept {
    return left.event_id == right.event_id && left.event_type == right.event_type &&
           left.source == right.source && same_optional(left.source_instance, right.source_instance) &&
           same_optional(left.occurred_at, right.occurred_at) && left.source_sequence == right.source_sequence &&
           same_optional(left.operation_id, right.operation_id) &&
           same_optional(left.correlation_id, right.correlation_id) &&
           same_optional(left.payload, right.payload) && left.quality_status == right.quality_status &&
           left.schema_version == right.schema_version && same_optional(left.reason_code, right.reason_code) &&
           same_optional(left.reason_text, right.reason_text);
}

Event make_event(std::string_view event_id, std::string_view operation_id,
                 std::string_view correlation_id, std::uint64_t sequence,
                 std::string_view payload = "{\"status\":\"ACCEPTED\"}",
                 std::string_view event_type = "OPERATION_STATUS_UPDATED") {
    return Event{event_id, event_type, "lid-manager", "boot-7", std::nullopt,
                 "2026-11-05T10:00:00Z", sequence, operation_id, correlation_id,
                 payload, "VALID", "1.0", std::nullopt, std::nullopt};
}

class FakeOperationUpdateRepository final : public OperationUpdateRepository {
public:
    std::vector<OperationRecord> operations{};
    std::vector<Event> events{};
    SourceCursor stored_cursor{"lid-manager", std::string_view{"boot-7"}, std::uint64_t{10}};
    bool event_lookup_error{false};
    bool operation_lookup_error{false};
    OperationUpdateCommitStatus forced_commit_status{OperationUpdateCommitStatus::Applied};
    bool force_commit_status{false};
    int event_reads{0};
    int operation_reads{0};
    int commits{0};

    void add_operation(std::string_view operation_id, std::string_view correlation_id,
                       OperationLifecycleStatus status = OperationLifecycleStatus::Requested,
                       std::uint64_t revision = 1) {
        operations.push_back({ActionRequest{operation_id, correlation_id, "lid_manager", "close_lid", std::nullopt},
                              status, revision});
    }

    EventRepositoryLookupResult find_by_id(std::string_view event_id) noexcept override {
        ++event_reads;
        if (event_lookup_error) return {EventRepositoryLookupStatus::Error, {}};
        for (const Event& stored : events) {
            if (stored.event_id == event_id) return {EventRepositoryLookupStatus::Found, stored};
        }
        return {EventRepositoryLookupStatus::NotFound, {}};
    }

    OperationLookupResult find_operation(std::string_view operation_id) noexcept override {
        ++operation_reads;
        if (operation_lookup_error) return {OperationLookupStatus::Error, {}};
        for (const OperationRecord& operation : operations) {
            if (operation.request.operation_id == operation_id) {
                return {OperationLookupStatus::Found, operation};
            }
        }
        return {OperationLookupStatus::NotFound, {}};
    }

    OperationUpdateCommitStatus commit_operation_update(
        const OperationRecord& expected, OperationLifecycleStatus next_status,
        const Event& event, const SourceCursor& expected_cursor) noexcept override {
        ++commits;
        if (force_commit_status) return forced_commit_status;

        // Atomic repository check: another worker may have stored this event after F03.
        for (const Event& stored : events) {
            if (stored.event_id == event.event_id) {
                return same_event(stored, event) ? OperationUpdateCommitStatus::DuplicateEvent
                                                  : OperationUpdateCommitStatus::EventConflict;
            }
        }

        OperationRecord* current = nullptr;
        for (OperationRecord& operation : operations) {
            if (operation.request.operation_id == expected.request.operation_id) {
                current = &operation;
                break;
            }
        }
        if (current == nullptr) return OperationUpdateCommitStatus::OperationNotFound;
        if (current->revision != expected.revision || current->status != expected.status) {
            return OperationUpdateCommitStatus::OperationChanged;
        }

        if (stored_cursor.source != expected_cursor.source ||
            !same_optional(stored_cursor.source_instance, expected_cursor.source_instance) ||
            stored_cursor.last_sequence != expected_cursor.last_sequence) {
            return OperationUpdateCommitStatus::SourceCursorChanged;
        }

        // Fake transaction updates the record, saves the event and advances the source cursor together.
        current->status = next_status;
        ++current->revision;
        events.push_back(event);
        stored_cursor.last_sequence = event.source_sequence;
        return OperationUpdateCommitStatus::Applied;
    }
};

SourceCursor cursor_at(std::uint64_t sequence) {
    return SourceCursor{"lid-manager", std::string_view{"boot-7"}, sequence};
}

void expect(bool condition, std::string_view name) {
    std::cout << (condition ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!condition) ++failures;
}

OperationUpdateResult apply(FakeOperationUpdateRepository& repository,
                            std::string_view event_id,
                            std::string_view operation_id,
                            std::string_view correlation_id,
                            std::uint64_t sequence,
                            OperationLifecycleStatus next_status,
                            const SourceCursor& cursor,
                            std::string_view payload = "{\"status\":\"UPDATED\"}") {
    const OperationUpdate update{make_event(event_id, operation_id, correlation_id, sequence, payload), next_status};
    return handle_operation_update(update, cursor, repository);
}
}

int main() {
    // 1. Normal transition; status, event and source cursor are committed together.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-1", "command-1");
        const auto result = apply(repository, "event-11", "operation-1", "command-1", 11,
                                  OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::Applied, "REQUESTED -> ACCEPTED is applied");
        expect(result.resulting_status == OperationLifecycleStatus::Accepted && result.revision == 2,
               "successful result reports next status and incremented revision");
        expect(repository.operations[0].status == OperationLifecycleStatus::Accepted &&
               repository.operations[0].revision == 2, "operation record is updated once");
        expect(repository.events.size() == 1 && repository.stored_cursor.last_sequence == 11,
               "event and source cursor are committed with the operation update");
    }

    // 2. The allowed path through start and cancellation follows the approved matrix.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-2", "command-2");
        auto cursor = cursor_at(10);
        auto result = apply(repository, "event-21", "operation-2", "command-2", 11,
                            OperationLifecycleStatus::Accepted, cursor);
        expect(result.status == OperationUpdateResultStatus::Applied, "cancellation scenario accepts request");
        cursor.last_sequence = 11;
        result = apply(repository, "event-22", "operation-2", "command-2", 12,
                       OperationLifecycleStatus::Started, cursor);
        expect(result.status == OperationUpdateResultStatus::Applied, "ACCEPTED -> STARTED is applied");
        cursor.last_sequence = 12;
        result = apply(repository, "event-23", "operation-2", "command-2", 13,
                       OperationLifecycleStatus::CancelRequested, cursor);
        expect(result.status == OperationUpdateResultStatus::Applied, "STARTED -> CANCEL_REQUESTED is applied");
        cursor.last_sequence = 13;
        result = apply(repository, "event-24", "operation-2", "command-2", 14,
                       OperationLifecycleStatus::Cancelled, cursor);
        expect(result.status == OperationUpdateResultStatus::Applied &&
               repository.operations[0].status == OperationLifecycleStatus::Cancelled,
               "CANCEL_REQUESTED -> CANCELLED is applied");
        expect(repository.operations[0].revision == 5, "each accepted lifecycle transition increments revision once");
    }

    // 3. Duplicate event is idempotent; receiver-assigned received_at may differ.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-3", "command-3");
        const Event first = make_event("event-31", "operation-3", "command-3", 11);
        const auto applied = handle_operation_update(
            OperationUpdate{first, OperationLifecycleStatus::Accepted}, cursor_at(10), repository);
        Event replay = first;
        replay.received_at = "later-received-time";
        const auto repeated = handle_operation_update(
            OperationUpdate{replay, OperationLifecycleStatus::Accepted}, cursor_at(11), repository);
        expect(applied.status == OperationUpdateResultStatus::Applied &&
               repeated.status == OperationUpdateResultStatus::Duplicate,
               "exact replay is recognized even when receiver timestamp differs");
        expect(repository.commits == 1 && repository.operations[0].revision == 2,
               "duplicate event does not repeat the commit");
    }

    // 4. Same event ID with different content is a conflict.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-4", "command-4");
        const Event first = make_event("event-41", "operation-4", "command-4", 11, "first");
        (void)handle_operation_update(OperationUpdate{first, OperationLifecycleStatus::Accepted}, cursor_at(10), repository);
        const Event changed = make_event("event-41", "operation-4", "command-4", 11, "changed");
        const auto result = handle_operation_update(
            OperationUpdate{changed, OperationLifecycleStatus::Accepted}, cursor_at(11), repository);
        expect(result.status == OperationUpdateResultStatus::EventConflict,
               "same event ID with different content is rejected as conflict");
        expect(repository.operations[0].revision == 2 && repository.commits == 1,
               "event conflict does not change operation");
    }

    // 5. Malformed envelope and wrong event type are rejected before any write.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-5", "command-5");
        Event malformed = make_event("event-51", "operation-5", "command-5", 11);
        malformed.correlation_id = std::nullopt;
        auto result = handle_operation_update(OperationUpdate{malformed, OperationLifecycleStatus::Accepted},
                                              cursor_at(10), repository);
        expect(result.status == OperationUpdateResultStatus::InvalidEvent, "missing correlation ID is rejected");
        Event wrong_type = make_event("event-52", "operation-5", "command-5", 11, "{}", "SUBSYSTEM_STATUS_UPDATED");
        result = handle_operation_update(OperationUpdate{wrong_type, OperationLifecycleStatus::Accepted},
                                         cursor_at(10), repository);
        expect(result.status == OperationUpdateResultStatus::WrongEventType, "non-operation event type is rejected");
        Event low_quality = make_event("event-520", "operation-5", "command-5", 11);
        low_quality.quality_status = "UNKNOWN";
        result = handle_operation_update(OperationUpdate{low_quality, OperationLifecycleStatus::Accepted},
                                         cursor_at(10), repository);
        expect(result.status == OperationUpdateResultStatus::EventNotReliable,
               "UNKNOWN event quality is not accepted as operation confirmation");
        result = apply(repository, "event-53", "operation-5", "command-5", 11,
                       static_cast<OperationLifecycleStatus>(255), cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::InvalidStatus, "unknown lifecycle value is rejected");
        expect(repository.commits == 0 && repository.events.empty(), "invalid updates have no persistence side effects");
    }

    // 6. F04 rejects old and unordered events.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-6", "command-6");
        auto result = apply(repository, "event-61", "operation-6", "command-6", 9,
                            OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::StaleEvent, "regressed source sequence is rejected");
        result = apply(repository, "event-62", "operation-6", "command-6", 10,
                       OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::EventOrderUnknown, "repeated source sequence is unordered");
        Event no_sequence = make_event("event-63", "operation-6", "command-6", 11);
        no_sequence.source_sequence = std::nullopt;
        result = handle_operation_update(OperationUpdate{no_sequence, OperationLifecycleStatus::Accepted},
                                         cursor_at(10), repository);
        expect(result.status == OperationUpdateResultStatus::EventOrderUnknown,
               "missing source sequence cannot establish event order");
        SourceCursor wrong_source{"other-source", std::string_view{"boot-7"}, 10};
        result = apply(repository, "event-64", "operation-6", "command-6", 11,
                       OperationLifecycleStatus::Accepted, wrong_source);
        expect(result.status == OperationUpdateResultStatus::EventOrderUnknown, "different source is unordered");
        SourceCursor wrong_session{"lid-manager", std::string_view{"other-boot"}, 10};
        result = apply(repository, "event-65", "operation-6", "command-6", 11,
                       OperationLifecycleStatus::Accepted, wrong_session);
        expect(result.status == OperationUpdateResultStatus::EventOrderUnknown, "different source session is unordered");
        expect(repository.commits == 0 && repository.events.empty(), "stale/unordered events never commit");
    }

    // 7. Operation ID and correlation ID must point to the exact saved operation.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-7", "command-7");
        auto result = apply(repository, "event-71", "operation-missing", "command-7", 11,
                            OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::OperationNotFound, "unknown operation ID is rejected");
        result = apply(repository, "event-72", "operation-7", "different-command", 11,
                       OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::CorrelationMismatch,
               "correlation ID from another command is rejected");
        expect(repository.commits == 0, "foreign operation updates do not commit");
    }

    // 8. Illegal jumps, terminal statuses and OUTCOME_UNKNOWN are frozen.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-8", "command-8");
        auto result = apply(repository, "event-81", "operation-8", "command-8", 11,
                            OperationLifecycleStatus::Started, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::TransitionNotAllowed,
               "REQUESTED -> STARTED cannot skip ACCEPTED");
        repository.operations[0].status = OperationLifecycleStatus::Completed;
        result = apply(repository, "event-82", "operation-8", "command-8", 11,
                       OperationLifecycleStatus::Failed, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::TransitionNotAllowed,
               "terminal COMPLETED status cannot be changed by ordinary event");
        repository.operations[0].status = OperationLifecycleStatus::OutcomeUnknown;
        result = apply(repository, "event-83", "operation-8", "command-8", 11,
                       OperationLifecycleStatus::Completed, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::TransitionNotAllowed,
               "OUTCOME_UNKNOWN cannot be resolved by late ordinary update");
        expect(repository.commits == 0, "forbidden status jumps never commit");
    }

    // 9. Independent operations may be updated in sequence order with one source cursor.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-a", "command-a");
        repository.add_operation("operation-b", "command-b");
        auto result = apply(repository, "event-91", "operation-a", "command-a", 11,
                            OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::Applied, "first parallel operation is updated");
        result = apply(repository, "event-92", "operation-b", "command-b", 12,
                       OperationLifecycleStatus::Failed, cursor_at(11));
        expect(result.status == OperationUpdateResultStatus::Applied, "second parallel operation is independently updated");
        expect(repository.operations[0].status == OperationLifecycleStatus::Accepted &&
               repository.operations[1].status == OperationLifecycleStatus::Failed,
               "parallel operation IDs keep separate lifecycle records");
    }

    // 10. Repository failures and concurrent writers fail closed; there is no blind retry.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-10", "command-10");
        repository.event_lookup_error = true;
        auto result = apply(repository, "event-101", "operation-10", "command-10", 11,
                            OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::RepositoryError,
               "event repository lookup error is not treated as new event");
        repository.event_lookup_error = false;
        repository.operation_lookup_error = true;
        result = apply(repository, "event-102", "operation-10", "command-10", 11,
                       OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::RepositoryError,
               "operation lookup error blocks update");
        repository.operation_lookup_error = false;
        repository.force_commit_status = true;
        repository.forced_commit_status = OperationUpdateCommitStatus::OperationChanged;
        result = apply(repository, "event-103", "operation-10", "command-10", 11,
                       OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::ConcurrentOperationUpdate,
               "concurrent operation revision change is reported, not overwritten");
        repository.forced_commit_status = OperationUpdateCommitStatus::SourceCursorChanged;
        result = apply(repository, "event-104", "operation-10", "command-10", 11,
                       OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::ConcurrentSourceUpdate,
               "concurrent source cursor change is reported, not overwritten");
        repository.forced_commit_status = OperationUpdateCommitStatus::Error;
        result = apply(repository, "event-105", "operation-10", "command-10", 11,
                       OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::RepositoryError,
               "atomic repository failure is propagated");
        expect(repository.operations[0].status == OperationLifecycleStatus::Requested &&
               repository.operations[0].revision == 1 && repository.events.empty(),
               "failed/concurrent commit leaves stored state unchanged in fake");
    }

    // 11. Unknown commit enum values are not treated as success.
    {
        FakeOperationUpdateRepository repository;
        repository.add_operation("operation-11", "command-11");
        repository.force_commit_status = true;
        repository.forced_commit_status = static_cast<OperationUpdateCommitStatus>(255);
        const auto result = apply(repository, "event-111", "operation-11", "command-11", 11,
                                  OperationLifecycleStatus::Accepted, cursor_at(10));
        expect(result.status == OperationUpdateResultStatus::RepositoryError,
               "unknown commit status fails closed");
    }

    std::cout << (failures == 0 ? "All tests passed" : "Test failures: ") << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
