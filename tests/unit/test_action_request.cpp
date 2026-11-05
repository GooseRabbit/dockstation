#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "core/operations.hpp"

using namespace dockstation::core;

namespace {

int failures = 0;

class FakeOperationIdProvider final : public OperationIdProvider {
public:
    std::string_view next_id{"op-001"};
    bool available{true};
    int calls{0};

    bool next_operation_id(std::string_view& output) noexcept override {
        ++calls;
        if (!available) {
            return false;
        }
        output = next_id;
        return true;
    }
};

void expect(bool condition, std::string_view name) {
    std::cout << (condition ? "PASS" : "FAIL") << " - " << name << '\n';
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    const ActionPlan valid_plan{"lid_manager", "close_lid", "{\"mode\":\"normal\"}", "event-100"};
    FakeOperationIdProvider provider;

    // 1. Валидный план превращается в запрос с выданным ID.
    const auto created = create_action_request(valid_plan, provider);
    expect(created.status == ActionRequestStatus::Created, "valid plan creates request");
    expect(static_cast<bool>(created), "created result converts to success");
    expect(created.request.operation_id == "op-001", "provider ID is copied into request view");
    expect(provider.calls == 1, "provider is called once for a valid plan");

    // 2. Все логические поля и связь с исходным событием сохраняются без преобразования.
    expect(created.request.correlation_id == valid_plan.correlation_id, "correlation ID is preserved");
    expect(created.request.target_module == valid_plan.target_module, "target module is preserved");
    expect(created.request.action == valid_plan.action, "action name is preserved");
    expect(created.request.parameters == valid_plan.parameters, "opaque parameters are preserved");

    // 3. Отсутствие параметров допустимо: модуль может иметь действие без аргументов.
    const ActionPlan no_parameters{"power_manager", "read_status", std::nullopt, "event-101"};
    provider.next_id = "op-002";
    const auto without_parameters = create_action_request(no_parameters, provider);
    expect(without_parameters.status == ActionRequestStatus::Created &&
               !without_parameters.request.parameters.has_value(),
           "parameters may be absent");
    const ActionPlan empty_parameters{"power_manager", "set_mode", std::string_view{""}, "event-103"};
    provider.next_id = "op-002-empty-payload";
    const auto with_empty_parameters = create_action_request(empty_parameters, provider);
    expect(with_empty_parameters.request.parameters.has_value() &&
               with_empty_parameters.request.parameters->empty(),
           "present but empty parameters remain distinct from absent parameters");

    // 4. Пустой адресат, действие или correlation_id блокируют создание запроса.
    const ActionPlan empty_module{"", "close_lid", std::nullopt, "event-102"};
    const int calls_before_invalid = provider.calls;
    expect(create_action_request(empty_module, provider).status == ActionRequestStatus::InvalidPlan,
           "empty target module is rejected");
    expect(provider.calls == calls_before_invalid, "invalid plan does not request an operation ID");

    const ActionPlan empty_action{"lid_manager", "", std::nullopt, "event-102"};
    const int calls_before_empty_action = provider.calls;
    expect(create_action_request(empty_action, provider).status == ActionRequestStatus::InvalidPlan,
           "empty action is rejected");
    expect(provider.calls == calls_before_empty_action, "empty action does not request an operation ID");

    const ActionPlan empty_correlation{"lid_manager", "close_lid", std::nullopt, ""};
    expect(create_action_request(empty_correlation, provider).status == ActionRequestStatus::InvalidPlan,
           "empty correlation ID is rejected");

    // 5. Ошибка поставщика ID блокирует формирование запроса.
    provider.available = false;
    expect(create_action_request(valid_plan, provider).status ==
               ActionRequestStatus::OperationIdUnavailable,
           "unavailable ID provider blocks request creation");

    // 6. Пустой ID при true — ошибка контракта поставщика.
    provider.available = true;
    provider.next_id = "";
    expect(create_action_request(valid_plan, provider).status ==
               ActionRequestStatus::InvalidOperationId,
           "empty ID from provider is rejected");

    // 7. Повторный вызов с тем же планом использует новый ID поставщика, не переиспользуя старый.
    provider.next_id = "op-003";
    const auto first_repeat = create_action_request(valid_plan, provider);
    provider.next_id = "op-004";
    const auto second_repeat = create_action_request(valid_plan, provider);
    expect(first_repeat.request.operation_id != second_repeat.request.operation_id,
           "repeated plan gets distinct provider IDs");
    expect(first_repeat.request.operation_id == "op-003", "first repeated request uses first new ID");
    expect(second_repeat.request.operation_id == "op-004", "second repeated request uses second new ID");

    // 8. Функция не меняет входной план.
    expect(valid_plan.target_module == "lid_manager" && valid_plan.action == "close_lid",
           "input plan action fields remain unchanged");
    expect(valid_plan.correlation_id == "event-100" && valid_plan.parameters.has_value(),
           "input plan context remains unchanged");

    std::cout << (failures == 0 ? "All tests passed" : "Test failures: ") << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
