#include "operations.hpp"

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

}  // namespace dockstation::core
