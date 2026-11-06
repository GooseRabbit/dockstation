#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include "core/state_transitions.hpp"
using namespace dockstation::core;
namespace {
int failures = 0;
int checks = 0;
void expect(std::string_view name, bool condition) {
    ++checks;
    std::cout << (condition ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!condition) ++failures;
}
Event make_event(std::string_view id, std::string_view type) {
    Event value{};
    value.event_id = id;
    value.event_type = type;
    value.source = "startup-service";
    value.received_at = "2026-11-05T12:00:00Z";
    value.source_sequence = 1;
    value.payload = "";
    value.quality_status = "VALID";
    value.schema_version = "1.0";
    return value;
}
struct FakeRepository final : StationStateRepository {
    StationStateRepositoryStatus response{StationStateRepositoryStatus::Committed};
    int calls{0};
    TransitionPlan received_plan{};
    TransitionEvidence received_evidence{};
    StationStateSnapshot received_next{};
    SourceCursor received_cursor{};
    StationStateRepositoryStatus commit_transition(
        const TransitionPlan& plan, const TransitionEvidence& evidence,
        const StationStateSnapshot& next_snapshot,
        const SourceCursor& expected_cursor) noexcept override {
        ++calls;
        received_plan = plan;
        received_evidence = evidence;
        received_next = next_snapshot;
        received_cursor = expected_cursor;
        return response;
    }
};
struct FakePublisher final : StateChangedPublisher {
    StateChangedPublishStatus response{StateChangedPublishStatus::Published};
    int calls{0};
    StateChanged received{};
    StateChangedPublishStatus publish(const StateChanged& event) noexcept override {
        ++calls;
        received = event;
        return response;
    }
};
TransitionEvidence evidence_for(Event event, TransitionContext context = {}) {
    return {event, context};
}

// Existing startup cases use the first event from this source/session.
// Tests for advanced, stale and mismatched cursors pass an explicit cursor.
TransitionCommitResult commit_with_initial_cursor(
    const TransitionPlan& plan, const TransitionEvidence& evidence,
    FakeRepository& repository, FakePublisher& publisher) {
    const SourceCursor cursor{
        evidence.event.source, evidence.event.source_instance, std::nullopt};
    return commit_transition(plan, evidence, cursor, repository, publisher);
}
}
int main() {
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto evidence = evidence_for(make_event("evt-1", "STARTUP_REQUESTED"));
        const TransitionPlan plan{StationState::PoweredOff, 0, StationState::Startup, "tr-1"};
        const auto result = commit_with_initial_cursor(plan, evidence, repository, publisher);
        expect("startup transition committed", result.status == TransitionCommitStatus::Committed);
        expect("startup revision increments exactly once", result.snapshot.revision == 1);
        expect("startup target is stored", result.snapshot.state == StationState::Startup);
        expect("transition ID is returned", result.snapshot.transition_id == "tr-1");
        expect("repository called exactly once", repository.calls == 1);
        expect("repository receives expected state and revision", repository.received_plan.expected_state == StationState::PoweredOff && repository.received_plan.expected_revision == 0);
        expect("repository receives next snapshot", repository.received_next.state == StationState::Startup && repository.received_next.revision == 1);
        expect("repository receives the cursor checked by F04", repository.received_cursor.source == "startup-service" && !repository.received_cursor.source_instance.has_value() && !repository.received_cursor.last_sequence.has_value());
        expect("publisher called only after repository commit", publisher.calls == 1);
        expect("StateChanged has correct state/revision/event IDs", publisher.received.old_state == StationState::PoweredOff && publisher.received.new_state == StationState::Startup && publisher.received.revision == 1 && publisher.received.event_id == "evt-1" && publisher.received.transition_id == "tr-1");
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        TransitionContext context{};
        context.configuration_validated = true;
        const auto evidence = evidence_for(make_event("evt-2", "INITIALIZATION_COMPLETED"), context);
        const TransitionPlan plan{StationState::Startup, 4, StationState::SelfTest, "tr-2"};
        const auto result = commit_with_initial_cursor(plan, evidence, repository, publisher);
        expect("validated initialization transition is accepted", result.status == TransitionCommitStatus::Committed);
        expect("initialization revision increments from expected value", result.snapshot.revision == 5);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        TransitionContext context{};
        context.diagnostic_report_complete = true;
        context.critical_checks_passed = true;
        context.safe_initial_condition_confirmed = true;
        context.recovery_status = RecoveryStatus::NotRequired;
        const auto evidence = evidence_for(make_event("evt-3", "SELF_TEST_COMPLETED"), context);
        const TransitionPlan plan{StationState::SelfTest, 8, StationState::Waiting, "tr-3"};
        const auto result = commit_with_initial_cursor(plan, evidence, repository, publisher);
        expect("fully evidenced self-test transition is accepted", result.status == TransitionCommitStatus::Committed);
        expect("waiting transition revision increments", result.snapshot.revision == 9);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto evidence = evidence_for(make_event("evt-4", "STARTUP_REQUESTED"));
        const TransitionPlan plan{StationState::PoweredOff, 0, StationState::Startup, "  "};
        const auto result = commit_with_initial_cursor(plan, evidence, repository, publisher);
        expect("empty transition ID is rejected", result.status == TransitionCommitStatus::InvalidPlan);
        expect("empty ID does not reach repository", repository.calls == 0);
        expect("empty ID does not publish", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto evidence = evidence_for(make_event("evt-5", "STARTUP_REQUESTED"));
        const TransitionPlan plan{StationState::PoweredOff, std::numeric_limits<std::uint64_t>::max(), StationState::Startup, "tr-5"};
        const auto result = commit_with_initial_cursor(plan, evidence, repository, publisher);
        expect("revision overflow is rejected", result.status == TransitionCommitStatus::InvalidPlan);
        expect("revision overflow does not persist", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        Event bad = make_event("", "STARTUP_REQUESTED");
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-6"},
            evidence_for(bad), repository, publisher);
        expect("invalid event envelope is rejected", result.status == TransitionCommitStatus::InvalidEvidence);
        expect("invalid event is not persisted", repository.calls == 0);
        expect("invalid event is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        Event bad_quality = make_event("evt-7", "STARTUP_REQUESTED");
        bad_quality.quality_status = "UNKNOWN";
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-7"},
            evidence_for(bad_quality), repository, publisher);
        expect("unknown-quality event is rejected", result.status == TransitionCommitStatus::InvalidEvidence);
        expect("unknown-quality event is not persisted", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        TransitionContext context{};
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::Startup, 0, StationState::SelfTest, "tr-8"},
            evidence_for(make_event("evt-8", "INITIALIZATION_COMPLETED"), context),
            repository, publisher);
        expect("F05 prerequisite failure is returned", result.status == TransitionCommitStatus::InvalidEvidence && result.reason_code == "CONFIGURATION_NOT_VALIDATED");
        expect("failed F05 decision is not persisted", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Waiting, "tr-9"},
            evidence_for(make_event("evt-9", "SELF_TEST_COMPLETED")), repository, publisher);
        expect("unapproved transition is rejected", result.status == TransitionCommitStatus::InvalidEvidence);
        expect("unapproved transition does not persist", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Waiting, "tr-10"},
            evidence_for(make_event("evt-10", "STARTUP_REQUESTED")), repository, publisher);
        expect("plan target must match F05 result", result.status == TransitionCommitStatus::InvalidPlan && result.reason_code == "TRANSITION_TARGET_MISMATCH");
        expect("mismatched plan is not persisted", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::Error;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-11"},
            evidence_for(make_event("evt-11", "STARTUP_REQUESTED")), repository, publisher);
        expect("repository error is returned", result.status == TransitionCommitStatus::RepositoryError);
        expect("publisher is not called after repository error", publisher.calls == 0);
        expect("repository was called once", repository.calls == 1);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::SnapshotChanged;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-12"},
            evidence_for(make_event("evt-12", "STARTUP_REQUESTED")), repository, publisher);
        expect("stale snapshot conflict is returned", result.status == TransitionCommitStatus::SnapshotChanged);
        expect("stale snapshot is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::DuplicateEvent;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-13"},
            evidence_for(make_event("evt-13", "STARTUP_REQUESTED")), repository, publisher);
        expect("duplicate event does not commit again", result.status == TransitionCommitStatus::DuplicateEvent);
        expect("duplicate event is not republished", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::EventConflict;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-14"},
            evidence_for(make_event("evt-14", "STARTUP_REQUESTED")), repository, publisher);
        expect("event ID conflict is returned", result.status == TransitionCommitStatus::EventConflict);
        expect("event conflict is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::TransitionIdConflict;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-15"},
            evidence_for(make_event("evt-15", "STARTUP_REQUESTED")), repository, publisher);
        expect("transition ID conflict is returned", result.status == TransitionCommitStatus::TransitionIdConflict);
        expect("transition ID conflict is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = static_cast<StationStateRepositoryStatus>(255);
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-16"},
            evidence_for(make_event("evt-16", "STARTUP_REQUESTED")), repository, publisher);
        expect("unknown repository status fails closed", result.status == TransitionCommitStatus::RepositoryError);
        expect("unknown repository status is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        publisher.response = StateChangedPublishStatus::Rejected;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-17"},
            evidence_for(make_event("evt-17", "STARTUP_REQUESTED")), repository, publisher);
        expect("publish rejection distinguishes committed state", result.status == TransitionCommitStatus::CommittedNotificationNotConfirmed);
        expect("publish rejection still returns committed snapshot", result.committed() && result.snapshot.state == StationState::Startup && result.snapshot.revision == 1);
        expect("commit is not repeated on publish rejection", repository.calls == 1 && publisher.calls == 1);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        publisher.response = StateChangedPublishStatus::Unknown;
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-18"},
            evidence_for(make_event("evt-18", "STARTUP_REQUESTED")), repository, publisher);
        expect("unknown publication status never claims publication", result.status == TransitionCommitStatus::CommittedNotificationNotConfirmed);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        Event event = make_event("evt-19", "STARTUP_REQUESTED");
        event.reason_code = "STARTUP_SOURCE_CONFIRMED";
        event.reason_text = "Startup requested by the local boot layer";
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-19"},
            evidence_for(event), repository, publisher);
        expect("reason metadata is committed and published", result.status == TransitionCommitStatus::Committed && publisher.received.reason_code == event.reason_code && publisher.received.reason_text == event.reason_text);
    }

    {
        FakeRepository repository;
        FakePublisher publisher;
        Event event = make_event("evt-20", "STARTUP_REQUESTED");
        event.source_instance = "boot-session-2";
        event.source_sequence = 11;
        const TransitionEvidence evidence = evidence_for(event);
        const SourceCursor cursor{"startup-service", std::string_view{"boot-session-2"}, 10};
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 10, StationState::Startup, "tr-20"},
            evidence, cursor, repository, publisher);
        expect("new sequence in matching source session is accepted", result.status == TransitionCommitStatus::Committed);
        expect("advanced cursor is passed unchanged to atomic repository", repository.received_cursor.source == cursor.source && repository.received_cursor.source_instance == cursor.source_instance && repository.received_cursor.last_sequence == cursor.last_sequence);
        expect("advanced source event is published only after commit", repository.calls == 1 && publisher.calls == 1);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const TransitionEvidence evidence = evidence_for(make_event("evt-22", "STARTUP_REQUESTED"));
        const SourceCursor cursor{"startup-service", std::nullopt, 1};
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 1, StationState::Startup, "tr-22"},
            evidence, cursor, repository, publisher);
        expect("repeated source sequence is rejected as unordered", result.status == TransitionCommitStatus::EventOrderUnknown);
        expect("repeated sequence never reaches repository or publisher", repository.calls == 0 && publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const TransitionEvidence evidence = evidence_for(make_event("evt-21", "STARTUP_REQUESTED"));
        const SourceCursor cursor{"startup-service", std::nullopt, 2};
        Event stale_event = evidence.event;
        stale_event.source_sequence = 1;
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 2, StationState::Startup, "tr-21"},
            evidence_for(stale_event), cursor, repository, publisher);
        expect("regressed source sequence is rejected as stale", result.status == TransitionCommitStatus::StaleEvent && result.reason_code == "STALE_EVENT");
        expect("stale event never reaches repository or publisher", repository.calls == 0 && publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const TransitionEvidence evidence = evidence_for(make_event("evt-23", "STARTUP_REQUESTED"));
        const SourceCursor cursor{"different-source", std::nullopt, std::nullopt};
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-23"},
            evidence, cursor, repository, publisher);
        expect("mismatched source is rejected as unordered", result.status == TransitionCommitStatus::EventOrderUnknown);
        expect("mismatched source never reaches repository", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        Event event = make_event("evt-24", "STARTUP_REQUESTED");
        event.source_instance = "boot-session-new";
        const TransitionEvidence evidence = evidence_for(event);
        const SourceCursor cursor{"startup-service", std::string_view{"boot-session-old"}, 20};
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 20, StationState::Startup, "tr-24"},
            evidence, cursor, repository, publisher);
        expect("unconfirmed source-session change is rejected", result.status == TransitionCommitStatus::EventOrderUnknown);
        expect("session mismatch never reaches repository", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        Event event = make_event("evt-25", "STARTUP_REQUESTED");
        event.source_sequence.reset();
        const TransitionEvidence evidence = evidence_for(event);
        const SourceCursor cursor{"startup-service", std::nullopt, std::nullopt};
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-25"},
            evidence, cursor, repository, publisher);
        expect("missing source sequence is rejected as unordered", result.status == TransitionCommitStatus::EventOrderUnknown);
        expect("missing sequence never reaches repository", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::SourceCursorChanged;
        const TransitionEvidence evidence = evidence_for(make_event("evt-26", "STARTUP_REQUESTED"));
        const auto result = commit_with_initial_cursor(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-26"},
            evidence, repository, publisher);
        expect("atomic source-cursor conflict is reported distinctly", result.status == TransitionCommitStatus::SourceCursorChanged && result.reason_code == "SOURCE_CURSOR_CHANGED");
        expect("source-cursor conflict is never published", repository.calls == 1 && publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        Event event = make_event("evt-27", "STARTUP_REQUESTED");
        event.source_sequence = 0;
        const TransitionEvidence evidence = evidence_for(event);
        const SourceCursor cursor{"startup-service", std::nullopt, std::nullopt};
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-27"},
            evidence, cursor, repository, publisher);
        expect("zero is accepted as the first source sequence", result.status == TransitionCommitStatus::Committed);
        expect("first zero sequence reaches the repository unchanged", repository.calls == 1 && repository.received_evidence.event.source_sequence == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        Event event = make_event("evt-28", "STARTUP_REQUESTED");
        event.source_sequence = std::numeric_limits<std::uint64_t>::max();
        const TransitionEvidence evidence = evidence_for(event);
        const SourceCursor cursor{"startup-service", std::nullopt,
                                  std::numeric_limits<std::uint64_t>::max() - 1U};
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 9, StationState::Startup, "tr-28"},
            evidence, cursor, repository, publisher);
        expect("maximum source sequence is accepted when strictly advanced", result.status == TransitionCommitStatus::Committed);
        expect("maximum sequence is passed to repository without arithmetic overflow", repository.calls == 1 && repository.received_evidence.event.source_sequence == std::numeric_limits<std::uint64_t>::max());
    }
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
