#include "state_transitions.hpp"
#include "event_validation.hpp"
#include <limits>
namespace dockstation::core {
namespace {
constexpr TransitionResult allowed(StationState next_state) noexcept {
    return {TransitionStatus::Allowed, next_state, {}, "Transition is allowed"};
}
constexpr TransitionResult rejected(std::string_view code,
                                     std::string_view text) noexcept {
    return {TransitionStatus::Rejected, std::nullopt, code, text};
}
}  // namespace
TransitionResult resolve_transition(
    StationState current_state,
    const Event& event,
    const TransitionContext& context) noexcept {
    // ВЫКЛЮЧЕНА -> ЗАПУСК: запуск инициируется стартовым программным слоем.
    if (current_state == StationState::PoweredOff &&
        event.event_type == "STARTUP_REQUESTED") {
        return allowed(StationState::Startup);
    }
    // ЗАПУСК -> САМОПРОВЕРКА: нельзя продолжать без проверенной конфигурации.
    if (current_state == StationState::Startup &&
        event.event_type == "INITIALIZATION_COMPLETED") {
        if (!context.configuration_validated) {
            return rejected("CONFIGURATION_NOT_VALIDATED",
                            "Stored configuration has not been positively validated");
        }
        return allowed(StationState::SelfTest);
    }
    // САМОПРОВЕРКА -> ОЖИДАНИЕ: нужны полный положительный отчёт,
    // подтверждённое безопасное исходное состояние и отсутствие восстановления.
    if (current_state == StationState::SelfTest &&
        event.event_type == "SELF_TEST_COMPLETED") {
        if (!context.diagnostic_report_complete) {
            return rejected("DIAGNOSTIC_REPORT_INCOMPLETE",
                            "The complete diagnostic report has not been received");
        }
        if (!context.critical_checks_passed) {
            return rejected("CRITICAL_CHECKS_NOT_PASSED",
                            "One or more critical checks are not positively passed");
        }
        if (!context.safe_initial_condition_confirmed) {
            return rejected("SAFE_INITIAL_CONDITION_UNCONFIRMED",
                            "The safe initial station condition is not confirmed");
        }
        // UNKNOWN и REQUIRED одинаково блокируют переход: восстановление должно быть
        // явно исключено до перехода в ОЖИДАНИЕ.
        if (context.recovery_status != RecoveryStatus::NotRequired) {
            return rejected("RECOVERY_REQUIRED",
                            "Recovery and equipment reconciliation are required before waiting");
        }
        return allowed(StationState::Waiting);
    }
    // Все остальные пары state/event пока не входят в согласованный первый срез.
    return rejected("TRANSITION_NOT_APPROVED",
                    "No approved transition exists for this state and event pair");
}


namespace {
bool has_non_whitespace(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' &&
            ch != '\f' && ch != '\v') return true;
    }
    return false;
}
TransitionCommitResult commit_result(TransitionCommitStatus status,
                                     std::string_view code = {},
                                     std::string_view text = {}) noexcept {
    return {status, {}, code, text};
}
}  // namespace

TransitionCommitResult commit_transition(
    const TransitionPlan& plan,
    const TransitionEvidence& evidence,
    StationStateRepository& repository,
    StateChangedPublisher& publisher) noexcept {
    // Не принимаем пустой ID записи истории и не допускаем переполнения revision.
    if (!has_non_whitespace(plan.transition_id) ||
        plan.expected_revision == std::numeric_limits<std::uint64_t>::max()) {
        return commit_result(TransitionCommitStatus::InvalidPlan, "INVALID_TRANSITION_PLAN",
                             "Transition ID is required and revision must be incrementable");
    }

    // Проверяем общий конверт события; сама функция не разбирает payload.
    const EventValidationResult validation = validate_event(evidence.event);
    if (!validation.valid) {
        return commit_result(TransitionCommitStatus::InvalidEvidence, "INVALID_EVENT",
                             validation.message);
    }
    // Переходы глобального состояния принимаются только по достоверному событию.
    if (evidence.event.quality_status != "VALID") {
        return commit_result(TransitionCommitStatus::InvalidEvidence, "EVENT_NOT_RELIABLE",
                             "Event quality_status must be VALID");
    }

    // Повторно вызываем F05, чтобы входной план не мог обойти согласованные правила.
    const TransitionResult decision = resolve_transition(
        plan.expected_state, evidence.event, evidence.context);
    if (!decision) {
        return commit_result(TransitionCommitStatus::InvalidEvidence,
                             decision.reason_code, decision.reason_text);
    }
    if (!decision.next_state.has_value() || *decision.next_state != plan.next_state) {
        return commit_result(TransitionCommitStatus::InvalidPlan, "TRANSITION_TARGET_MISMATCH",
                             "Plan target does not match the approved F05 transition");
    }

    // Формируем новую версию, которая будет сохранена вместе с историей.
    const StationStateSnapshot next_snapshot{
        plan.next_state, plan.expected_revision + 1U, plan.transition_id};

    // Репозиторий обязан атомарно проверить состояние/версию, дедупликацию event_id
    // и уникальность transition_id, затем сохранить snapshot, историю и исходный Event.
    const StationStateRepositoryStatus saved = repository.commit_transition(
        plan, evidence, next_snapshot);
    switch (saved) {
        case StationStateRepositoryStatus::Committed:
            break;
        case StationStateRepositoryStatus::DuplicateEvent:
            return commit_result(TransitionCommitStatus::DuplicateEvent, "DUPLICATE_EVENT",
                                 "This event has already been committed");
        case StationStateRepositoryStatus::EventConflict:
            return commit_result(TransitionCommitStatus::EventConflict, "EVENT_ID_CONFLICT",
                                 "event_id is already stored with different content");
        case StationStateRepositoryStatus::TransitionIdConflict:
            return commit_result(TransitionCommitStatus::TransitionIdConflict,
                                 "TRANSITION_ID_CONFLICT",
                                 "transition_id is already used by another transition");
        case StationStateRepositoryStatus::SnapshotChanged:
            return commit_result(TransitionCommitStatus::SnapshotChanged, "STATE_SNAPSHOT_CHANGED",
                                 "Stored state or revision no longer matches the transition plan");
        case StationStateRepositoryStatus::Error:
        default:
            return commit_result(TransitionCommitStatus::RepositoryError, "STATE_REPOSITORY_ERROR",
                                 "Repository did not confirm an atomic transition commit");
    }

    // StateChanged публикуется только после подтверждённой фиксации в хранилище.
    const StateChanged notification{
        plan.expected_state, plan.next_state, next_snapshot.revision,
        evidence.event.event_id, plan.transition_id,
        evidence.event.reason_code, evidence.event.reason_text};
    const StateChangedPublishStatus published = publisher.publish(notification);
    if (published != StateChangedPublishStatus::Published) {
        // Состояние уже сохранено. Нельзя выдавать это за ошибку записи или повторять commit.
        return {TransitionCommitStatus::CommittedNotificationNotConfirmed,
                next_snapshot, "STATE_CHANGED_NOT_CONFIRMED",
                "State is committed; StateChanged publication was not confirmed"};
    }
    return {TransitionCommitStatus::Committed, next_snapshot, {},
            "Transition committed and published"};
}

}  // namespace dockstation::core
