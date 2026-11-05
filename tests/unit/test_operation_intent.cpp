#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "core/operations.hpp"

using namespace dockstation::core;

namespace {

int failures = 0;

// Имитирует ответы StateRepository, но не моделирует реальную долговременную запись.
class FakeStateRepository final : public StateRepository {
public:
    OperationIntentSaveStatus next_status{OperationIntentSaveStatus::Saved};
    int calls{0};
    ActionRequest received{};

    OperationIntentSaveStatus save_operation_intent(const ActionRequest& request) noexcept override {
        ++calls;
        received = request;
        return next_status;
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
    const ActionRequest request{
        "operation-001", "event-100", "lid_manager", "close_lid", "{\"mode\":\"normal\"}"};
    FakeStateRepository repository;

    // 1. Новое намерение считается сохранённым только после положительного ответа репозитория.
    const auto saved = persist_operation_intent(request, repository);
    expect(saved.status == PersistStatus::Persisted, "new intent is reported as persisted");
    expect(saved.may_dispatch(), "newly persisted intent may be dispatched once by F08");
    expect(repository.calls == 1, "repository is called once for a valid request");

    // 2. Все поля передаются хранилищу без потери или преобразования.
    expect(repository.received.operation_id == request.operation_id, "operation ID is persisted");
    expect(repository.received.correlation_id == request.correlation_id, "correlation ID is persisted");
    expect(repository.received.target_module == request.target_module, "logical target is persisted");
    expect(repository.received.action == request.action, "logical action is persisted");
    expect(repository.received.parameters == request.parameters, "opaque parameters are persisted");

    // 3. Уже сохранённый точный запрос не даёт повторного разрешения на dispatch.
    repository.next_status = OperationIntentSaveStatus::AlreadySavedSame;
    const auto already_saved = persist_operation_intent(request, repository);
    expect(already_saved.status == PersistStatus::AlreadyPersisted,
           "identical existing intent is reported as already persisted");
    expect(!already_saved.may_dispatch(), "already persisted intent must not be dispatched again");

    // 4. Конфликт и ошибка репозитория блокируют дальнейшую отправку.
    repository.next_status = OperationIntentSaveStatus::Conflict;
    const auto conflict = persist_operation_intent(request, repository);
    expect(conflict.status == PersistStatus::Conflict && !conflict.may_dispatch(),
           "operation ID conflict blocks dispatch");

    repository.next_status = OperationIntentSaveStatus::Error;
    const auto error = persist_operation_intent(request, repository);
    expect(error.status == PersistStatus::RepositoryError && !error.may_dispatch(),
           "repository error blocks dispatch");

    // 5. Некорректный запрос блокируется до обращения к хранилищу.
    const int calls_before_invalid = repository.calls;
    const ActionRequest no_operation_id{"", "event-101", "lid_manager", "close_lid", std::nullopt};
    expect(persist_operation_intent(no_operation_id, repository).status == PersistStatus::InvalidRequest,
           "empty operation ID is rejected");
    const ActionRequest no_correlation_id{"operation-002", "", "lid_manager", "close_lid", std::nullopt};
    expect(persist_operation_intent(no_correlation_id, repository).status == PersistStatus::InvalidRequest,
           "empty correlation ID is rejected");
    const ActionRequest no_target{"operation-003", "event-103", "", "close_lid", std::nullopt};
    expect(persist_operation_intent(no_target, repository).status == PersistStatus::InvalidRequest,
           "empty target module is rejected");
    const ActionRequest no_action{"operation-004", "event-104", "lid_manager", "", std::nullopt};
    expect(persist_operation_intent(no_action, repository).status == PersistStatus::InvalidRequest,
           "empty action is rejected");
    expect(repository.calls == calls_before_invalid,
           "invalid requests are rejected without repository calls");

    // 6. Отсутствующий payload разрешён; присутствующий пустой payload отличается от отсутствующего.
    repository.next_status = OperationIntentSaveStatus::Saved;
    const ActionRequest no_parameters{"operation-005", "event-105", "power_manager", "read_status", std::nullopt};
    const auto no_parameters_result = persist_operation_intent(no_parameters, repository);
    expect(no_parameters_result.status == PersistStatus::Persisted &&
               !repository.received.parameters.has_value(),
           "absent parameters are preserved");
    const ActionRequest empty_parameters{"operation-006", "event-106", "power_manager", "set_mode", std::string_view{""}};
    (void)persist_operation_intent(empty_parameters, repository);
    expect(repository.received.parameters.has_value() && repository.received.parameters->empty(),
           "present empty parameters are preserved");

    // 7. Неизвестное значение статуса репозитория закрывает обработку.
    repository.next_status = static_cast<OperationIntentSaveStatus>(255);
    const auto unknown = persist_operation_intent(request, repository);
    expect(unknown.status == PersistStatus::RepositoryError && !unknown.may_dispatch(),
           "unknown repository status blocks dispatch");

    // 8. Входной запрос не изменяется.
    expect(request.operation_id == "operation-001" && request.action == "close_lid" &&
               request.correlation_id == "event-100",
           "input request remains unchanged");

    std::cout << (failures == 0 ? "All tests passed" : "Test failures: ") << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
