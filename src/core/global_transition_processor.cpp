#include "global_transition_processor.hpp"
namespace dockstation::core {
namespace {
bool has_non_whitespace(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' &&
            ch != '\f' && ch != '\v') return true;
    }
    return false;
}
bool is_supported_pair(const Event& event, EventCategory category) noexcept {
    return (event.event_type == "STARTUP_REQUESTED" && category == EventCategory::Command) ||
           (event.event_type == "INITIALIZATION_COMPLETED" && category == EventCategory::Subsystem) ||
           (event.event_type == "SELF_TEST_COMPLETED" && category == EventCategory::Subsystem);
}
}
GlobalTransitionProcessor::GlobalTransitionProcessor(
    StationStateSnapshotReader& snapshot_reader,
    TransitionContextProvider& context_provider,
    TransitionIdProvider& transition_id_provider,
    StationStateRepository& repository,
    StateChangedPublisher& publisher) noexcept
    : snapshot_reader_(snapshot_reader), context_provider_(context_provider),
      transition_id_provider_(transition_id_provider), repository_(repository),
      publisher_(publisher) {}
EventProcessorStatus GlobalTransitionProcessor::process(
    const Event& event, EventCategory category,
    const SourceCursor& expected_cursor) noexcept {
    if (!is_supported_pair(event, category)) return EventProcessorStatus::Unsupported;
    if (event.quality_status != "VALID") return EventProcessorStatus::Rejected;
    // Отсутствующий/повреждённый snapshot не заменяется POWERED_OFF.
    const SnapshotReadResult snapshot_result = get_snapshot(snapshot_reader_);
    if (snapshot_result.status != SnapshotReadStatus::Available ||
        !snapshot_result.snapshot.has_value()) return EventProcessorStatus::Error;
    const StationStateSnapshot& snapshot = *snapshot_result.snapshot;
    TransitionContext context{};
    if (event.event_type != "STARTUP_REQUESTED") {
        const TransitionContextReadResult context_result =
            context_provider_.read_transition_context(event);
        switch (context_result.status) {
            case TransitionContextReadStatus::Available:
                context = context_result.context;
                break;
            case TransitionContextReadStatus::Unavailable:
                // Частичные значения от недоступного источника отбрасываются.
                context = TransitionContext{};
                break;
            case TransitionContextReadStatus::Error:
            default:
                return EventProcessorStatus::Error;
        }
    }
    const TransitionResult decision = resolve_transition(snapshot.state, event, context);
    if (!decision || !decision.next_state.has_value()) return EventProcessorStatus::Rejected;
    std::string_view transition_id{};
    if (!transition_id_provider_.next_transition_id(transition_id) ||
        !has_non_whitespace(transition_id)) return EventProcessorStatus::Error;
    const TransitionPlan plan{snapshot.state, snapshot.revision,
                              *decision.next_state, transition_id};
    const TransitionEvidence evidence{event, context};
    // Передаём F10 тот же immutable cursor, который проверил F04 в F13.
    const TransitionCommitResult committed = commit_transition(
        plan, evidence, expected_cursor, repository_, publisher_);
    switch (committed.status) {
        case TransitionCommitStatus::Committed:
            return EventProcessorStatus::Processed;
        case TransitionCommitStatus::CommittedNotificationNotConfirmed:
            return EventProcessorStatus::CommittedNotificationNotConfirmed;
        case TransitionCommitStatus::DuplicateEvent:
            return EventProcessorStatus::Duplicate;
        case TransitionCommitStatus::EventConflict:
            return EventProcessorStatus::EventConflict;
        case TransitionCommitStatus::SnapshotChanged:
        case TransitionCommitStatus::SourceCursorChanged:
            return EventProcessorStatus::ConcurrentUpdate;
        case TransitionCommitStatus::InvalidEvidence:
            return EventProcessorStatus::Rejected;
        case TransitionCommitStatus::TransitionIdConflict:
        case TransitionCommitStatus::StaleEvent:
        case TransitionCommitStatus::EventOrderUnknown:
        case TransitionCommitStatus::InvalidPlan:
        case TransitionCommitStatus::RepositoryError:
        default:
            // F13 проверил тот же event/cursor; неожиданный иной F04-результат F10 — несогласованность.
            return EventProcessorStatus::Error;
    }
}
}  // namespace dockstation::core
