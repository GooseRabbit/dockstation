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

}  // namespace dockstation::core
