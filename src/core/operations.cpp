#include "operations.hpp"

#include <limits>

#include "event_validation.hpp"

namespace dockstation::core {
namespace {

bool same_optional_text(const std::optional<std::string_view>& left,
                        const std::optional<std::string_view>& right) noexcept {
    return left.has_value() == right.has_value() &&
           (!left.has_value() || *left == *right);
}

bool same_action_request(const ActionRequest& left, const ActionRequest& right) noexcept {
    // Сверяем все поля запроса: один operation_id не может менять адресата, действие или параметры.
    return left.operation_id == right.operation_id &&
           left.correlation_id == right.correlation_id &&
           left.target_module == right.target_module &&
           left.action == right.action &&
           same_optional_text(left.parameters, right.parameters);
}


bool is_known_operation_status(OperationLifecycleStatus status) noexcept {
    switch (status) {
        case OperationLifecycleStatus::Requested:
        case OperationLifecycleStatus::Accepted:
        case OperationLifecycleStatus::Started:
        case OperationLifecycleStatus::Completed:
        case OperationLifecycleStatus::Failed:
        case OperationLifecycleStatus::CancelRequested:
        case OperationLifecycleStatus::Cancelled:
        case OperationLifecycleStatus::OutcomeUnknown:
            return true;
    }
    return false;
}

bool is_allowed_operation_transition(OperationLifecycleStatus current,
                                     OperationLifecycleStatus next) noexcept {
    // Матрица lifecycle из docs/06 §19.1.
    switch (current) {
        case OperationLifecycleStatus::Requested:
            return next == OperationLifecycleStatus::Accepted ||
                   next == OperationLifecycleStatus::Failed ||
                   next == OperationLifecycleStatus::OutcomeUnknown;
        case OperationLifecycleStatus::Accepted:
            return next == OperationLifecycleStatus::Started ||
                   next == OperationLifecycleStatus::Failed ||
                   next == OperationLifecycleStatus::CancelRequested ||
                   next == OperationLifecycleStatus::OutcomeUnknown;
        case OperationLifecycleStatus::Started:
            return next == OperationLifecycleStatus::Completed ||
                   next == OperationLifecycleStatus::Failed ||
                   next == OperationLifecycleStatus::CancelRequested ||
                   next == OperationLifecycleStatus::OutcomeUnknown;
        case OperationLifecycleStatus::CancelRequested:
            return next == OperationLifecycleStatus::Cancelled ||
                   next == OperationLifecycleStatus::Failed ||
                   next == OperationLifecycleStatus::OutcomeUnknown;
        case OperationLifecycleStatus::Completed:
        case OperationLifecycleStatus::Failed:
        case OperationLifecycleStatus::Cancelled:
        case OperationLifecycleStatus::OutcomeUnknown:
            return false;
    }
    return false;
}

OperationUpdateResult update_result(OperationUpdateResultStatus status,
                                    OperationLifecycleStatus resulting_status = OperationLifecycleStatus::Requested,
                                    std::uint64_t revision = 0) noexcept {
    return {status, resulting_status, revision};
}

}  // namespace

ActionRequestResult create_action_request(
    const ActionPlan& plan,
    OperationIdProvider& id_provider) noexcept {
    // Нельзя сформировать запрос без логического получателя, действия и связи с источником.
    if (plan.target_module.empty() || plan.action.empty() || plan.correlation_id.empty()) {
        return {ActionRequestStatus::InvalidPlan, {}};
    }

    // ID выдаёт отдельный порт: алгоритм генерации не привязывается к SSM.
    std::string_view operation_id{};
    if (!id_provider.next_operation_id(operation_id)) {
        return {ActionRequestStatus::OperationIdUnavailable, {}};
    }

    // Пустой ID при заявленном успехе — нарушение контракта поставщика ID.
    if (operation_id.empty()) {
        return {ActionRequestStatus::InvalidOperationId, {}};
    }

    // Запрос сохраняет непрозрачные логические данные плана; физическое исполнение — следующий этап.
    const ActionRequest request{
        operation_id,
        plan.correlation_id,
        plan.target_module,
        plan.action,
        plan.parameters,
    };
    return {ActionRequestStatus::Created, request};
}


bool PersistResult::authorizes(const ActionRequest& request) const noexcept {
    return status == PersistStatus::Persisted && same_action_request(persisted_request, request);
}

PersistResult persist_operation_intent(
    const ActionRequest& request,
    StateRepository& repository) noexcept {
    // Запрос может поступить не только напрямую из F06, поэтому обязательные поля проверяются повторно.
    if (request.operation_id.empty() || request.correlation_id.empty() ||
        request.target_module.empty() || request.action.empty()) {
        return {PersistStatus::InvalidRequest, {}};
    }

    // StateRepository обязан атомарно обеспечить уникальность operation_id и устойчивую фиксацию.
    const OperationIntentSaveStatus save_status = repository.save_operation_intent(request);

    switch (save_status) {
        case OperationIntentSaveStatus::Saved:
            // Только впервые сохранённая запись даёт право перейти к отправке в F08.
            return {PersistStatus::Persisted, request};
        case OperationIntentSaveStatus::AlreadySavedSame:
            // Повторное сохранение не доказывает, что исполнитель ещё не получал команду.
            return {PersistStatus::AlreadyPersisted, {}};
        case OperationIntentSaveStatus::Conflict:
            // Один operation_id с разными данными — конфликт; физическое действие блокируется.
            return {PersistStatus::Conflict, {}};
        case OperationIntentSaveStatus::Error:
            // Неизвестный исход записи трактуется fail-closed.
            return {PersistStatus::RepositoryError, {}};
    }

    // Неизвестное значение enum также не считается подтверждением сохранения.
    return {PersistStatus::RepositoryError, {}};
}

DispatchResult dispatch_action_request(
    const ActionRequest& request,
    const PersistResult& persistence,
    ActionDispatcher& dispatcher) noexcept {
    // Повторно проверяем обязательные поля: F08 может вызываться не только из основного сценария.
    if (request.operation_id.empty() || request.correlation_id.empty() ||
        request.target_module.empty() || request.action.empty()) {
        return {DispatchStatus::InvalidRequest};
    }

    // Нельзя отправлять запрос, если F07 не подтвердила впервые созданную запись.
    if (persistence.status != PersistStatus::Persisted) {
        return {DispatchStatus::PersistenceNotConfirmed};
    }

    // Защищаемся от подмены запроса после сохранения: подтверждение должно совпадать по всем полям.
    if (!persistence.authorizes(request)) {
        return {DispatchStatus::PersistenceReceiptMismatch};
    }

    // ActionDispatcher только передаёт запрос менеджеру; физическое действие подтверждается отдельно.
    const ActionDispatchPortStatus port_status = dispatcher.dispatch(request);
    switch (port_status) {
        case ActionDispatchPortStatus::Accepted:
            return {DispatchStatus::Accepted};
        case ActionDispatchPortStatus::AlreadyKnown:
            return {DispatchStatus::AlreadyKnown};
        case ActionDispatchPortStatus::Rejected:
            return {DispatchStatus::Rejected};
        case ActionDispatchPortStatus::Conflict:
            return {DispatchStatus::Conflict};
        case ActionDispatchPortStatus::Unknown:
            return {DispatchStatus::OutcomeUnknown};
    }

    // Неизвестное значение статуса fail-closed трактуется как неопределённый результат.
    return {DispatchStatus::OutcomeUnknown};
}


OperationUpdateResult handle_operation_update(
    const OperationUpdate& update,
    const SourceCursor& source_cursor,
    OperationUpdateRepository& repository) noexcept {
    // Повторно проверяем конверт и обязательные operation_id/correlation_id.
    const EventValidationPolicy validation_policy{false, false, true};
    if (!validate_event(update.event, validation_policy)) {
        return update_result(OperationUpdateResultStatus::InvalidEvent);
    }

    // Неизвестное, устаревшее или недостоверное качество не подтверждает операцию.
    if (update.event.quality_status != "VALID") {
        return update_result(OperationUpdateResultStatus::EventNotReliable);
    }

    // Принимается только зарегистрированный тип обновления lifecycle.
    if (update.event.event_type != "OPERATION_STATUS_UPDATED") {
        return update_result(OperationUpdateResultStatus::WrongEventType);
    }
    if (!is_known_operation_status(update.next_status)) {
        return update_result(OperationUpdateResultStatus::InvalidStatus);
    }

    // F03: точный повтор не меняет запись повторно; тот же ID с другими данными — конфликт.
    const DuplicateEventResult duplicate = check_duplicate_event(update.event, repository);
    switch (duplicate.status) {
        case DuplicateEventStatus::Duplicate:
            return update_result(OperationUpdateResultStatus::Duplicate);
        case DuplicateEventStatus::Conflict:
            return update_result(OperationUpdateResultStatus::EventConflict);
        case DuplicateEventStatus::RepositoryError:
            return update_result(OperationUpdateResultStatus::RepositoryError);
        case DuplicateEventStatus::New:
            break;
    }

    // F04: событие должно иметь доказуемый порядок в той же сессии источника.
    const EventFreshnessResult freshness = check_event_freshness(update.event, source_cursor);
    if (freshness.status == EventFreshnessStatus::Stale) {
        return update_result(OperationUpdateResultStatus::StaleEvent);
    }
    if (freshness.status != EventFreshnessStatus::Current) {
        return update_result(OperationUpdateResultStatus::EventOrderUnknown);
    }

    // Событие обновляет только уже существующую операцию, созданную ранее через F07.
    const OperationLookupResult lookup = repository.find_operation(*update.event.operation_id);
    switch (lookup.status) {
        case OperationLookupStatus::NotFound:
            return update_result(OperationUpdateResultStatus::OperationNotFound);
        case OperationLookupStatus::Error:
            return update_result(OperationUpdateResultStatus::RepositoryError);
        case OperationLookupStatus::Found:
            break;
        default:
            return update_result(OperationUpdateResultStatus::RepositoryError);
    }

    const OperationRecord& record = lookup.record;
    if (record.request.operation_id != *update.event.operation_id ||
        record.request.correlation_id.empty()) {
        return update_result(OperationUpdateResultStatus::RepositoryError);
    }
    if (record.request.correlation_id != *update.event.correlation_id) {
        return update_result(OperationUpdateResultStatus::CorrelationMismatch,
                             record.status, record.revision);
    }
    if (record.revision == std::numeric_limits<std::uint64_t>::max()) {
        return update_result(OperationUpdateResultStatus::RepositoryError,
                             record.status, record.revision);
    }
    if (!is_known_operation_status(record.status) ||
        !is_allowed_operation_transition(record.status, update.next_status)) {
        return update_result(OperationUpdateResultStatus::TransitionNotAllowed,
                             record.status, record.revision);
    }

    // Атомарно фиксируем lifecycle, event_id/event и курсор. При гонке не повторяем запись вслепую.
    const OperationUpdateCommitStatus committed = repository.commit_operation_update(
        record, update.next_status, update.event, source_cursor);
    switch (committed) {
        case OperationUpdateCommitStatus::Applied:
            return update_result(OperationUpdateResultStatus::Applied,
                                 update.next_status, record.revision + 1U);
        case OperationUpdateCommitStatus::DuplicateEvent:
            return update_result(OperationUpdateResultStatus::Duplicate,
                                 record.status, record.revision);
        case OperationUpdateCommitStatus::EventConflict:
            return update_result(OperationUpdateResultStatus::EventConflict,
                                 record.status, record.revision);
        case OperationUpdateCommitStatus::OperationChanged:
            return update_result(OperationUpdateResultStatus::ConcurrentOperationUpdate,
                                 record.status, record.revision);
        case OperationUpdateCommitStatus::SourceCursorChanged:
            return update_result(OperationUpdateResultStatus::ConcurrentSourceUpdate,
                                 record.status, record.revision);
        case OperationUpdateCommitStatus::OperationNotFound:
            return update_result(OperationUpdateResultStatus::OperationNotFound);
        case OperationUpdateCommitStatus::Error:
            return update_result(OperationUpdateResultStatus::RepositoryError);
    }
    return update_result(OperationUpdateResultStatus::RepositoryError);
}

}  // namespace dockstation::core
