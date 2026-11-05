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
    StationStateRepositoryStatus commit_transition(
        const TransitionPlan& plan, const TransitionEvidence& evidence,
        const StationStateSnapshot& next_snapshot) noexcept override {
        ++calls;
        received_plan = plan;
        received_evidence = evidence;
        received_next = next_snapshot;
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
}
int main() {
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto evidence = evidence_for(make_event("evt-1", "STARTUP_REQUESTED"));
        const TransitionPlan plan{StationState::PoweredOff, 0, StationState::Startup, "tr-1"};
        const auto result = commit_transition(plan, evidence, repository, publisher);
        expect("startup transition committed", result.status == TransitionCommitStatus::Committed);
        expect("startup revision increments exactly once", result.snapshot.revision == 1);
        expect("startup target is stored", result.snapshot.state == StationState::Startup);
        expect("transition ID is returned", result.snapshot.transition_id == "tr-1");
        expect("repository called exactly once", repository.calls == 1);
        expect("repository receives expected state and revision", repository.received_plan.expected_state == StationState::PoweredOff && repository.received_plan.expected_revision == 0);
        expect("repository receives next snapshot", repository.received_next.state == StationState::Startup && repository.received_next.revision == 1);
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
        const auto result = commit_transition(plan, evidence, repository, publisher);
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
        const auto result = commit_transition(plan, evidence, repository, publisher);
        expect("fully evidenced self-test transition is accepted", result.status == TransitionCommitStatus::Committed);
        expect("waiting transition revision increments", result.snapshot.revision == 9);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto evidence = evidence_for(make_event("evt-4", "STARTUP_REQUESTED"));
        const TransitionPlan plan{StationState::PoweredOff, 0, StationState::Startup, "  "};
        const auto result = commit_transition(plan, evidence, repository, publisher);
        expect("empty transition ID is rejected", result.status == TransitionCommitStatus::InvalidPlan);
        expect("empty ID does not reach repository", repository.calls == 0);
        expect("empty ID does not publish", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto evidence = evidence_for(make_event("evt-5", "STARTUP_REQUESTED"));
        const TransitionPlan plan{StationState::PoweredOff, std::numeric_limits<std::uint64_t>::max(), StationState::Startup, "tr-5"};
        const auto result = commit_transition(plan, evidence, repository, publisher);
        expect("revision overflow is rejected", result.status == TransitionCommitStatus::InvalidPlan);
        expect("revision overflow does not persist", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        Event bad = make_event("", "STARTUP_REQUESTED");
        const auto result = commit_transition(
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
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-7"},
            evidence_for(bad_quality), repository, publisher);
        expect("unknown-quality event is rejected", result.status == TransitionCommitStatus::InvalidEvidence);
        expect("unknown-quality event is not persisted", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        TransitionContext context{};
        const auto result = commit_transition(
            TransitionPlan{StationState::Startup, 0, StationState::SelfTest, "tr-8"},
            evidence_for(make_event("evt-8", "INITIALIZATION_COMPLETED"), context),
            repository, publisher);
        expect("F05 prerequisite failure is returned", result.status == TransitionCommitStatus::InvalidEvidence && result.reason_code == "CONFIGURATION_NOT_VALIDATED");
        expect("failed F05 decision is not persisted", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Waiting, "tr-9"},
            evidence_for(make_event("evt-9", "SELF_TEST_COMPLETED")), repository, publisher);
        expect("unapproved transition is rejected", result.status == TransitionCommitStatus::InvalidEvidence);
        expect("unapproved transition does not persist", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Waiting, "tr-10"},
            evidence_for(make_event("evt-10", "STARTUP_REQUESTED")), repository, publisher);
        expect("plan target must match F05 result", result.status == TransitionCommitStatus::InvalidPlan && result.reason_code == "TRANSITION_TARGET_MISMATCH");
        expect("mismatched plan is not persisted", repository.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::Error;
        const auto result = commit_transition(
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
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-12"},
            evidence_for(make_event("evt-12", "STARTUP_REQUESTED")), repository, publisher);
        expect("stale snapshot conflict is returned", result.status == TransitionCommitStatus::SnapshotChanged);
        expect("stale snapshot is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::DuplicateEvent;
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-13"},
            evidence_for(make_event("evt-13", "STARTUP_REQUESTED")), repository, publisher);
        expect("duplicate event does not commit again", result.status == TransitionCommitStatus::DuplicateEvent);
        expect("duplicate event is not republished", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::EventConflict;
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-14"},
            evidence_for(make_event("evt-14", "STARTUP_REQUESTED")), repository, publisher);
        expect("event ID conflict is returned", result.status == TransitionCommitStatus::EventConflict);
        expect("event conflict is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = StationStateRepositoryStatus::TransitionIdConflict;
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-15"},
            evidence_for(make_event("evt-15", "STARTUP_REQUESTED")), repository, publisher);
        expect("transition ID conflict is returned", result.status == TransitionCommitStatus::TransitionIdConflict);
        expect("transition ID conflict is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        repository.response = static_cast<StationStateRepositoryStatus>(255);
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-16"},
            evidence_for(make_event("evt-16", "STARTUP_REQUESTED")), repository, publisher);
        expect("unknown repository status fails closed", result.status == TransitionCommitStatus::RepositoryError);
        expect("unknown repository status is not published", publisher.calls == 0);
    }
    {
        FakeRepository repository;
        FakePublisher publisher;
        publisher.response = StateChangedPublishStatus::Rejected;
        const auto result = commit_transition(
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
        const auto result = commit_transition(
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
        const auto result = commit_transition(
            TransitionPlan{StationState::PoweredOff, 0, StationState::Startup, "tr-19"},
            evidence_for(event), repository, publisher);
        expect("reason metadata is committed and published", result.status == TransitionCommitStatus::Committed && publisher.received.reason_code == event.reason_code && publisher.received.reason_text == event.reason_text);
    }
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
