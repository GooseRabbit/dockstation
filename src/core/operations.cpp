#include "operations.hpp"

namespace dockstation::core {

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


PersistResult persist_operation_intent(
    const ActionRequest& request,
    StateRepository& repository) noexcept {
    // Запрос может поступить не только напрямую из F06, поэтому обязательные поля проверяются повторно.
    if (request.operation_id.empty() || request.correlation_id.empty() ||
        request.target_module.empty() || request.action.empty()) {
        return {PersistStatus::InvalidRequest};
    }

    // StateRepository обязан атомарно обеспечить уникальность operation_id и устойчивую фиксацию.
    const OperationIntentSaveStatus save_status = repository.save_operation_intent(request);

    switch (save_status) {
        case OperationIntentSaveStatus::Saved:
            // Только впервые сохранённая запись даёт право перейти к отправке в F08.
            return {PersistStatus::Persisted};
        case OperationIntentSaveStatus::AlreadySavedSame:
            // Повторное сохранение не доказывает, что исполнитель ещё не получал команду.
            return {PersistStatus::AlreadyPersisted};
        case OperationIntentSaveStatus::Conflict:
            // Один operation_id с разными данными — конфликт; физическое действие блокируется.
            return {PersistStatus::Conflict};
        case OperationIntentSaveStatus::Error:
            // Неизвестный исход записи трактуется fail-closed.
            return {PersistStatus::RepositoryError};
    }

    // Неизвестное значение enum также не считается подтверждением сохранения.
    return {PersistStatus::RepositoryError};
}

}  // namespace dockstation::core
