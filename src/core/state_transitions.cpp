#include "state_transitions.hpp"
#include "event_validation.hpp"
#include <limits>
#include <utility>
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



namespace {
bool is_known_station_state(StationState state) noexcept {
    switch (state) {
        case StationState::PoweredOff:
        case StationState::Startup:
        case StationState::SelfTest:
        case StationState::Waiting:
        case StationState::Detection:
        case StationState::Authentication:
        case StationState::Authorization:
        case StationState::ReceptionPreparation:
        case StationState::LidOpening:
        case StationState::LandingGuidance:
        case StationState::LandingConfirmation:
        case StationState::LidClosing:
        case StationState::EnclosedService:
        case StationState::MissionWaiting:
        case StationState::TakeoffPreparation:
        case StationState::OpeningForTakeoff:
        case StationState::Takeoff:
        case StationState::SelfCleaning:
        case StationState::LimitedMode:
        case StationState::AwaitingIntervention:
        case StationState::Recovery:
        case StationState::Emergency:
            return true;
    }
    return false;
}

bool is_valid_snapshot(const StationStateSnapshot& snapshot) noexcept {
    if (!is_known_station_state(snapshot.state)) return false;
    if (snapshot.revision == 0) return snapshot.transition_id.empty();
    return has_non_whitespace(snapshot.transition_id);
}
}  // namespace

SnapshotReadResult get_snapshot(StationStateSnapshotReader& reader) noexcept {
    // Выполняем ровно одно чтение: повторять ошибочный запрос автоматически нельзя.
    const SnapshotReadPortResult read = reader.read_snapshot();

    switch (read.status) {
        case SnapshotReadPortStatus::Found:
            if (!read.snapshot.has_value() || !is_valid_snapshot(*read.snapshot)) {
                return {SnapshotReadStatus::InvalidSnapshot, std::nullopt};
            }
            // Возвращаем ту же revision и transition_id без их изменения.
            return {SnapshotReadStatus::Available, read.snapshot};

        case SnapshotReadPortStatus::NotFound:
            return {SnapshotReadStatus::NotFound, std::nullopt};

        case SnapshotReadPortStatus::Error:
            return {SnapshotReadStatus::ReadError, std::nullopt};
    }

    // Неизвестный код порта трактуется как ошибка, а не как успешное чтение.
    return {SnapshotReadStatus::ReadError, std::nullopt};
}


namespace {
bool is_valid_history_record(const TransitionHistoryRecord& record) noexcept {
    // История содержит только подтверждённые переходы с корректными ссылками.
    if (!is_known_station_state(record.from_state) ||
        !is_known_station_state(record.to_state) ||
        record.from_state == record.to_state ||
        record.revision == 0 ||
        !has_non_whitespace(record.event_id) ||
        !has_non_whitespace(record.transition_id)) {
        return false;
    }
    // Причина записывается только как согласованная пара; пустая причина не считается известной.
    if (record.reason_code.has_value() != record.reason_text.has_value()) return false;
    if (record.reason_code.has_value() &&
        (!has_non_whitespace(*record.reason_code) ||
         !has_non_whitespace(*record.reason_text))) {
        return false;
    }
    return true;
}

bool matches_history_filter(const TransitionHistoryRecord& record,
                            const TransitionHistoryFilter& filter) noexcept {
    if (filter.minimum_revision.has_value() &&
        record.revision < *filter.minimum_revision) return false;
    if (filter.maximum_revision.has_value() &&
        record.revision > *filter.maximum_revision) return false;
    if (filter.resulting_state.has_value() &&
        record.to_state != *filter.resulting_state) return false;
    return true;
}

TransitionHistoryResult history_result(TransitionHistoryStatus status) noexcept {
    return {status, {}};
}
}  // namespace

TransitionHistoryResult get_transition_history(
    const TransitionHistoryFilter& filter,
    TransitionHistoryReader& reader) noexcept {
    // Отклоняем противоречивый диапазон и неизвестное значение enum до чтения хранилища.
    if ((filter.minimum_revision.has_value() &&
         filter.maximum_revision.has_value() &&
         *filter.minimum_revision > *filter.maximum_revision) ||
        (filter.resulting_state.has_value() &&
         !is_known_station_state(*filter.resulting_state))) {
        return history_result(TransitionHistoryStatus::InvalidFilter);
    }

    // Выполняем ровно один запрос. Частичный набор нельзя выдавать за полную историю.
    TransitionHistoryPortResult read = reader.read_transition_history(filter);
    switch (read.status) {
        case TransitionHistoryPortStatus::Complete:
            break;
        case TransitionHistoryPortStatus::Incomplete:
            return history_result(TransitionHistoryStatus::Incomplete);
        case TransitionHistoryPortStatus::Error:
            return history_result(TransitionHistoryStatus::ReadError);
        default:
            return history_result(TransitionHistoryStatus::ReadError);
    }

    // Проверяем каждую запись, соответствие фильтру и строгий порядок revision.
    for (std::size_t i = 0; i < read.records.size(); ++i) {
        const TransitionHistoryRecord& record = read.records[i];
        if (!is_valid_history_record(record) ||
            !matches_history_filter(record, filter) ||
            (i > 0 && read.records[i - 1].revision >= record.revision)) {
            return history_result(TransitionHistoryStatus::InvalidHistory);
        }
    }

    if (read.records.empty()) return history_result(TransitionHistoryStatus::Empty);

    // Записи используют владеющие строки, поэтому результат не зависит от буфера reader.
    return {TransitionHistoryStatus::Available, std::move(read.records)};
}

}  // namespace dockstation::core
