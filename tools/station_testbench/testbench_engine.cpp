// DockStation StationStateManager host testbench bridge.
// Uses the real F01, F05, F10, F16 and F17 implementations from src/core.
// All repositories and publishers below are in-memory test doubles. No hardware is accessed.

#include "core/event_validation.hpp"
#include "core/event_processing.hpp"
#include "core/state_transitions.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dockstation::core;

namespace {

std::string_view state_name(StationState state) noexcept {
    switch (state) {
        case StationState::PoweredOff: return "POWERED_OFF";
        case StationState::Startup: return "STARTUP";
        case StationState::SelfTest: return "SELF_TEST";
        case StationState::Waiting: return "WAITING";
        case StationState::Detection: return "DETECTION";
        case StationState::Authentication: return "AUTHENTICATION";
        case StationState::Authorization: return "AUTHORIZATION";
        case StationState::ReceptionPreparation: return "RECEPTION_PREPARATION";
        case StationState::LidOpening: return "LID_OPENING";
        case StationState::LandingGuidance: return "LANDING_GUIDANCE";
        case StationState::LandingConfirmation: return "LANDING_CONFIRMATION";
        case StationState::LidClosing: return "LID_CLOSING";
        case StationState::EnclosedService: return "ENCLOSED_SERVICE";
        case StationState::MissionWaiting: return "MISSION_WAITING";
        case StationState::TakeoffPreparation: return "TAKEOFF_PREPARATION";
        case StationState::OpeningForTakeoff: return "OPENING_FOR_TAKEOFF";
        case StationState::Takeoff: return "TAKEOFF";
        case StationState::SelfCleaning: return "SELF_CLEANING";
        case StationState::LimitedMode: return "LIMITED_MODE";
        case StationState::AwaitingIntervention: return "AWAITING_INTERVENTION";
        case StationState::Recovery: return "RECOVERY";
        case StationState::Emergency: return "EMERGENCY";
    }
    return "UNKNOWN_STATE";
}

std::string_view commit_status_name(TransitionCommitStatus status) noexcept {
    switch (status) {
        case TransitionCommitStatus::Committed: return "COMMITTED";
        case TransitionCommitStatus::CommittedNotificationNotConfirmed:
            return "COMMITTED_NOTIFICATION_NOT_CONFIRMED";
        case TransitionCommitStatus::DuplicateEvent: return "DUPLICATE_EVENT";
        case TransitionCommitStatus::EventConflict: return "EVENT_CONFLICT";
        case TransitionCommitStatus::TransitionIdConflict: return "TRANSITION_ID_CONFLICT";
        case TransitionCommitStatus::SnapshotChanged: return "SNAPSHOT_CHANGED";
        case TransitionCommitStatus::SourceCursorChanged: return "SOURCE_CURSOR_CHANGED";
        case TransitionCommitStatus::StaleEvent: return "STALE_EVENT";
        case TransitionCommitStatus::EventOrderUnknown: return "EVENT_ORDER_UNKNOWN";
        case TransitionCommitStatus::InvalidPlan: return "INVALID_PLAN";
        case TransitionCommitStatus::InvalidEvidence: return "INVALID_EVIDENCE";
        case TransitionCommitStatus::RepositoryError: return "REPOSITORY_ERROR";
    }
    return "UNKNOWN_COMMIT_STATUS";
}

std::string_view event_category_name(EventCategory category) noexcept {
    switch (category) {
        case EventCategory::Subsystem: return "SUBSYSTEM";
        case EventCategory::Operation: return "OPERATION";
        case EventCategory::Safety: return "SAFETY";
        case EventCategory::Command: return "COMMAND";
        case EventCategory::Timer: return "TIMER";
        case EventCategory::Recovery: return "RECOVERY";
        case EventCategory::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

// In-memory implementation of the F03/F10/F16/F17 logical ports.
// It intentionally lasts only for the current testbench session.
class MemoryStationRepository final : public StationStateRepository,
                                      public StationStateSnapshotReader,
                                      public TransitionHistoryReader,
                                      public EventRepository {
public:
    MemoryStationRepository() { reset(); }

    void reset() {
        state_ = StationState::PoweredOff;
        revision_ = 0;
        transition_id_.clear();
        history_.clear();
        committed_event_ids_.clear();
        committed_event_types_.clear();
        committed_transition_ids_.clear();
        events_.clear();
        last_source_sequence_.reset();
    }

    [[nodiscard]] bool has_last_event() const noexcept {
        return !events_.empty();
    }

    [[nodiscard]] Event last_event() const noexcept {
        return events_.empty() ? Event{} : events_.back().as_event();
    }

    EventRepositoryLookupResult find_by_id(std::string_view event_id) noexcept override {
        for (const StoredEvent& stored : events_) {
            if (stored.event_id == event_id) {
                return {EventRepositoryLookupStatus::Found, stored.as_event()};
            }
        }
        return {EventRepositoryLookupStatus::NotFound, {}};
    }

    StationStateRepositoryStatus commit_transition(
        const TransitionPlan& plan,
        const TransitionEvidence& evidence,
        const StationStateSnapshot& next_snapshot,
        const SourceCursor& expected_cursor) noexcept override {
        // Compare-and-set semantics: a stale plan cannot overwrite a newer snapshot.
        if (plan.expected_state != state_ || plan.expected_revision != revision_) {
            return StationStateRepositoryStatus::SnapshotChanged;
        }

        // Simulate the repository's atomic compare-and-advance of this test source cursor.
        // A mismatched cursor means another accepted event changed the source position.
        if (expected_cursor.source != "station-testbench" ||
            expected_cursor.source_instance.has_value() ||
            expected_cursor.last_sequence != last_source_sequence_) {
            return StationStateRepositoryStatus::SourceCursorChanged;
        }

        // Reuse F03 at the storage boundary so duplicate IDs cannot be committed twice.
        const DuplicateEventResult duplicate = check_duplicate_event(evidence.event, *this);
        if (duplicate.status == DuplicateEventStatus::Duplicate) {
            return StationStateRepositoryStatus::DuplicateEvent;
        }
        if (duplicate.status == DuplicateEventStatus::Conflict) {
            return StationStateRepositoryStatus::EventConflict;
        }
        if (duplicate.status == DuplicateEventStatus::RepositoryError) {
            return StationStateRepositoryStatus::Error;
        }
        for (const auto& id : committed_transition_ids_) {
            if (id == plan.transition_id) {
                return StationStateRepositoryStatus::TransitionIdConflict;
            }
        }

        // Copy non-owning input strings before returning from the port.
        TransitionHistoryRecord record{};
        record.from_state = state_;
        record.to_state = next_snapshot.state;
        record.revision = next_snapshot.revision;
        record.event_id = std::string(evidence.event.event_id);
        record.transition_id = std::string(plan.transition_id);
        if (evidence.event.reason_code.has_value()) {
            record.reason_code = std::string(*evidence.event.reason_code);
            record.reason_text = std::string(*evidence.event.reason_text);
        }

        history_.push_back(std::move(record));
        events_.push_back(StoredEvent::copy_from(evidence.event));
        committed_event_ids_.emplace_back(evidence.event.event_id);
        committed_event_types_.emplace_back(evidence.event.event_type);
        committed_transition_ids_.emplace_back(plan.transition_id);
        state_ = next_snapshot.state;
        revision_ = next_snapshot.revision;
        transition_id_ = std::string(plan.transition_id);
        // Store the event sequence in the same in-memory commit as snapshot/history/event.
        last_source_sequence_ = evidence.event.source_sequence;
        return StationStateRepositoryStatus::Committed;
    }

    SnapshotReadPortResult read_snapshot() noexcept override {
        return {SnapshotReadPortStatus::Found,
                StationStateSnapshot{state_, revision_, transition_id_}};
    }

    TransitionHistoryPortResult read_transition_history(
        const TransitionHistoryFilter& filter) noexcept override {
        TransitionHistoryPortResult result{};
        result.status = TransitionHistoryPortStatus::Complete;
        for (const auto& record : history_) {
            if (filter.minimum_revision.has_value() &&
                record.revision < *filter.minimum_revision) continue;
            if (filter.maximum_revision.has_value() &&
                record.revision > *filter.maximum_revision) continue;
            if (filter.resulting_state.has_value() &&
                record.to_state != *filter.resulting_state) continue;
            result.records.push_back(record);
        }
        return result;
    }

private:
    struct StoredEvent {
        std::string event_id;
        std::string event_type;
        std::string source;
        std::string received_at;
        std::string payload;
        std::string quality_status;
        std::string schema_version;
        std::optional<std::uint64_t> source_sequence;

        static StoredEvent copy_from(const Event& event) {
            StoredEvent stored{};
            stored.event_id = std::string(event.event_id);
            stored.event_type = std::string(event.event_type);
            stored.source = std::string(event.source);
            stored.received_at = std::string(event.received_at);
            stored.payload = event.payload.has_value() ? std::string(*event.payload) : std::string{};
            stored.quality_status = std::string(event.quality_status);
            stored.schema_version = std::string(event.schema_version);
            stored.source_sequence = event.source_sequence;
            return stored;
        }

        [[nodiscard]] Event as_event() const noexcept {
            Event event{};
            event.event_id = event_id;
            event.event_type = event_type;
            event.source = source;
            event.received_at = received_at;
            event.payload = std::string_view(payload);
            event.quality_status = quality_status;
            event.schema_version = schema_version;
            event.source_sequence = source_sequence;
            return event;
        }
    };

    StationState state_{StationState::PoweredOff};
    std::uint64_t revision_{0};
    std::string transition_id_{};
    std::vector<TransitionHistoryRecord> history_{};
    std::vector<std::string> committed_event_ids_{};
    std::vector<std::string> committed_event_types_{};
    std::vector<std::string> committed_transition_ids_{};
    std::vector<StoredEvent> events_{};
    std::optional<std::uint64_t> last_source_sequence_{};
};

class MemoryPublisher final : public StateChangedPublisher {
public:
    StateChangedPublishStatus publish(const StateChanged&) noexcept override {
        return StateChangedPublishStatus::Published;
    }
};

void emit_readback(MemoryStationRepository& repository) {
    const SnapshotReadResult snapshot = get_snapshot(repository);
    if (snapshot.status == SnapshotReadStatus::Available && snapshot.snapshot.has_value()) {
        const auto& value = *snapshot.snapshot;
        std::cout << "SNAPSHOT\tAVAILABLE\t" << state_name(value.state)
                  << '\t' << value.revision << '\t' << value.transition_id << '\n';
    } else {
        std::cout << "SNAPSHOT\tERROR\tUNKNOWN\t0\t\n";
    }

    const TransitionHistoryResult history =
        get_transition_history(TransitionHistoryFilter{}, repository);
    if (history.status == TransitionHistoryStatus::Available ||
        history.status == TransitionHistoryStatus::Empty) {
        std::cout << "HISTORY\tAVAILABLE\t" << history.records.size() << '\n';
        for (const auto& record : history.records) {
            std::cout << "ROW\t" << record.revision << '\t'
                      << state_name(record.from_state) << '\t'
                      << state_name(record.to_state) << '\t'
                      << record.event_id << '\t' << record.transition_id << '\n';
        }
    } else {
        std::cout << "HISTORY\tERROR\t0\n";
    }
    std::cout << "END\n" << std::flush;
}

void emit_action(std::string_view status,
                 std::string_view reason_code,
                 std::string_view message,
                 MemoryStationRepository& repository) {
    std::cout << "ACTION\t" << status << '\t' << reason_code << '\t' << message << '\n';
    emit_readback(repository);
}

void attempt_transition(std::string_view event_type,
                        const TransitionContext& context,
                        MemoryStationRepository& repository,
                        MemoryPublisher& publisher,
                        std::uint64_t& event_counter,
                        std::uint64_t& transition_counter,
                        std::optional<std::uint64_t>& last_accepted_sequence) {
    const SnapshotReadResult current = get_snapshot(repository);
    if (current.status != SnapshotReadStatus::Available || !current.snapshot.has_value()) {
        emit_action("ERROR", "SNAPSHOT_UNAVAILABLE", "Current snapshot could not be read", repository);
        return;
    }

    // These strings own the memory referenced by Event's string_view fields until the call ends.
    ++event_counter;
    const std::string event_id = "tb-event-" + std::to_string(event_counter);
    const std::string received_at = "testbench-local";
    const std::string payload = "{}";
    Event event{};
    event.event_id = event_id;
    event.event_type = event_type;
    event.source = "station-testbench";
    event.received_at = received_at;
    event.source_sequence = event_counter;
    event.payload = std::string_view(payload);
    event.quality_status = "VALID";
    event.schema_version = "1";

    // F01: validate the common event envelope before any event processing.
    const EventValidationResult validation = validate_event(event);
    if (!validation.valid) {
        emit_action("REJECTED", "INVALID_EVENT", validation.message, repository);
        return;
    }

    // F02: identify the registered event category using an exact event_type match.
    const EventCategory category = classify_event(event);

    // F03: reject an already committed event ID or the same ID with changed content.
    const DuplicateEventResult duplicate = check_duplicate_event(event, repository);
    if (duplicate.status != DuplicateEventStatus::New) {
        const std::string_view status = duplicate.status == DuplicateEventStatus::Duplicate
            ? "DUPLICATE_EVENT" : (duplicate.status == DuplicateEventStatus::Conflict
                ? "EVENT_CONFLICT" : "EVENT_REPOSITORY_ERROR");
        emit_action(status, "F03_REJECTED", "Event was not passed to transition handling", repository);
        return;
    }

    // F04: compare source sequence against the last accepted event from this source/session.
    const SourceCursor cursor{"station-testbench", std::nullopt, last_accepted_sequence};
    const EventFreshnessResult freshness = check_event_freshness(event, cursor);
    if (freshness.status != EventFreshnessStatus::Current) {
        const std::string_view reason = freshness.status == EventFreshnessStatus::Stale
            ? "EVENT_STALE" : "EVENT_FRESHNESS_UNORDERED";
        emit_action("REJECTED", reason, "F04 did not confirm a current event", repository);
        return;
    }

    // F05: make a pure decision. Rejected transitions are not sent to F10.
    const TransitionResult decision =
        resolve_transition(current.snapshot->state, event, context);
    if (!decision) {
        emit_action("REJECTED", decision.reason_code, decision.reason_text, repository);
        return;
    }
    if (!decision.next_state.has_value()) {
        emit_action("REJECTED", "MISSING_TARGET_STATE", "F05 returned no target state", repository);
        return;
    }

    ++transition_counter;
    const std::string transition_id = "tb-transition-" + std::to_string(transition_counter);
    const TransitionPlan plan{current.snapshot->state,
                              current.snapshot->revision,
                              *decision.next_state,
                              transition_id};
    const TransitionEvidence evidence{event, context};

    // F10 repeats F01/F04/F05 checks and atomically commits the source cursor with the transition.
    const TransitionCommitResult committed =
        commit_transition(plan, evidence, cursor, repository, publisher);
    const bool success = committed.status == TransitionCommitStatus::Committed;
    if (committed.committed()) {
        last_accepted_sequence = event.source_sequence;
    }
    const std::string details = "F02=" + std::string(event_category_name(category)) +
        "; F01=VALID; F03=NEW; F04=CURRENT; F05=ALLOW; F10=" +
        std::string(commit_status_name(committed.status));
    emit_action(success ? "COMMITTED" : std::string_view(commit_status_name(committed.status)),
                committed.reason_code,
                success ? std::string_view(details) : committed.reason_text,
                repository);
}

void run_duplicate_check(MemoryStationRepository& repository, bool conflict) {
    if (!repository.has_last_event()) {
        emit_action("CHECK_UNAVAILABLE", "NO_SAVED_EVENT", "Сначала выполни хотя бы один успешный переход", repository);
        return;
    }
    Event candidate = repository.last_event();
    const std::string fresh_received_at = "testbench-received-again";
    const std::string conflicting_payload = "{\"changed\":true}";
    candidate.received_at = fresh_received_at; // F03 intentionally ignores receiver-assigned time.
    if (conflict) candidate.payload = std::string_view(conflicting_payload);
    const DuplicateEventResult result = check_duplicate_event(candidate, repository);
    if (!conflict && result.status == DuplicateEventStatus::Duplicate) {
        emit_action("DUPLICATE_DETECTED", "F03_DUPLICATE", "F03 correctly identified a repeated event", repository);
    } else if (conflict && result.status == DuplicateEventStatus::Conflict) {
        emit_action("CONFLICT_DETECTED", "F03_CONFLICT", "F03 detected reused event_id with changed content", repository);
    } else {
        emit_action("CHECK_FAILED", "F03_UNEXPECTED_RESULT", "F03 returned an unexpected duplicate-check result", repository);
    }
}

void run_freshness_check(MemoryStationRepository& repository,
                         const std::optional<std::uint64_t>& last_accepted_sequence,
                         bool stale) {
    if (!last_accepted_sequence.has_value()) {
        emit_action("CHECK_UNAVAILABLE", "NO_SEQUENCE_CURSOR", "Сначала выполни хотя бы один успешный переход", repository);
        return;
    }
    const std::uint64_t sequence = stale
        ? (*last_accepted_sequence == 0 ? 0 : *last_accepted_sequence - 1)
        : *last_accepted_sequence;
    Event candidate{};
    candidate.event_id = "tb-freshness-probe";
    candidate.event_type = "COMMAND_REQUESTED";
    candidate.source = "station-testbench";
    candidate.received_at = "testbench-probe";
    candidate.source_sequence = sequence;
    candidate.payload = std::string_view("{}");
    candidate.quality_status = "VALID";
    candidate.schema_version = "1";
    const EventValidationResult validation = validate_event(candidate);
    if (!validation.valid) {
        emit_action("CHECK_FAILED", "F01_INVALID_PROBE", validation.message, repository);
        return;
    }
    const SourceCursor cursor{"station-testbench", std::nullopt, last_accepted_sequence};
    const EventFreshnessResult result = check_event_freshness(candidate, cursor);
    if (stale && result.status == EventFreshnessStatus::Stale) {
        emit_action("STALE_DETECTED", "F04_STALE", "F04 correctly marked the regressed source sequence as stale", repository);
    } else if (!stale && result.status == EventFreshnessStatus::Unordered) {
        emit_action("UNORDERED_DETECTED", "F04_UNORDERED", "F04 correctly refused to order a repeated source sequence", repository);
    } else {
        emit_action("CHECK_FAILED", "F04_UNEXPECTED_RESULT", "F04 returned an unexpected freshness result", repository);
    }
}

}  // namespace

int main() {
    MemoryStationRepository repository;
    MemoryPublisher publisher;
    std::uint64_t event_counter = 0;
    std::uint64_t transition_counter = 0;
    std::optional<std::uint64_t> last_accepted_sequence{};
    std::string command;

    while (std::getline(std::cin, command)) {
        if (command == "QUIT") {
            std::cout << "BYE\nEND\n" << std::flush;
            return 0;
        }
        if (command == "STATE") {
            emit_readback(repository);
            continue;
        }
        if (command == "RESET") {
            // Simulator-only reset. It is not a station shutdown transition and does not touch hardware.
            repository.reset();
            event_counter = 0;
            transition_counter = 0;
            last_accepted_sequence.reset();
            std::cout << "ACTION\tRESET\tTESTBENCH_ONLY\tIn-memory test scenario was reset\n";
            emit_readback(repository);
            continue;
        }
        if (command == "STARTUP") {
            attempt_transition("STARTUP_REQUESTED", {}, repository, publisher,
                               event_counter, transition_counter, last_accepted_sequence);
            continue;
        }
        if (command.rfind("INITIALIZE|", 0) == 0) {
            TransitionContext context{};
            context.configuration_validated = command.substr(11) == "1";
            attempt_transition("INITIALIZATION_COMPLETED", context, repository, publisher,
                               event_counter, transition_counter, last_accepted_sequence);
            continue;
        }
        if (command.rfind("SELF_TEST|", 0) == 0) {
            // Format: SELF_TEST|diagnostic_report_complete|critical_checks_passed|
            // safe_initial_condition_confirmed|UNKNOWN/REQUIRED/NOT_REQUIRED
            std::vector<std::string> fields;
            std::size_t start = 10;
            while (start <= command.size()) {
                const std::size_t separator = command.find('|', start);
                fields.push_back(command.substr(start, separator == std::string::npos
                    ? std::string::npos : separator - start));
                if (separator == std::string::npos) break;
                start = separator + 1;
            }
            TransitionContext context{};
            context.diagnostic_report_complete = fields.size() > 0 && fields[0] == "1";
            context.critical_checks_passed = fields.size() > 1 && fields[1] == "1";
            context.safe_initial_condition_confirmed = fields.size() > 2 && fields[2] == "1";
            if (fields.size() > 3 && fields[3] == "NOT_REQUIRED") {
                context.recovery_status = RecoveryStatus::NotRequired;
            } else if (fields.size() > 3 && fields[3] == "REQUIRED") {
                context.recovery_status = RecoveryStatus::Required;
            } else {
                context.recovery_status = RecoveryStatus::Unknown;
            }
            attempt_transition("SELF_TEST_COMPLETED", context, repository, publisher,
                               event_counter, transition_counter, last_accepted_sequence);
            continue;
        }
        if (command == "SHUTDOWN") {
            // Intentionally tests the currently unsupported event; F05 must reject it.
            attempt_transition("SHUTDOWN_REQUESTED", {}, repository, publisher,
                               event_counter, transition_counter, last_accepted_sequence);
            continue;
        }
        if (command == "DUPLICATE") {
            run_duplicate_check(repository, false);
            continue;
        }
        if (command == "CONFLICT") {
            run_duplicate_check(repository, true);
            continue;
        }
        if (command == "STALE") {
            run_freshness_check(repository, last_accepted_sequence, true);
            continue;
        }
        if (command == "UNORDERED") {
            run_freshness_check(repository, last_accepted_sequence, false);
            continue;
        }

        emit_action("ERROR", "UNKNOWN_COMMAND", "Testbench command is not recognized", repository);
    }
    return 0;
}
