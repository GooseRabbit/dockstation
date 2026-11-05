#pragma once

#include <optional>
#include <string_view>

namespace dockstation::core {

// Логический план одного действия. Он не содержит команд GPIO или параметров драйвера.
// string_view не владеет памятью: строки должны оставаться действительными до сохранения запроса.
struct ActionPlan {
    std::string_view target_module;  // Логический получатель запроса, например идентификатор функционального модуля.
    std::string_view action;         // Логическое имя действия; каталог действий пока не утверждён.
    std::optional<std::string_view> parameters;  // Непрозрачные параметры; схема зависит от будущего контракта модуля.
    std::string_view correlation_id; // Связь с исходным событием/командой; формируется вызывающим слоем.
};

// Идентификатор выдаётся внешним поставщиком; формат и алгоритм здесь не закрепляются.
class OperationIdProvider {
public:
    virtual ~OperationIdProvider() = default;

    // При успехе записывает непустой ID в output. Выданное значение должно оставаться
    // действительным и уникальным относительно ранее выданных/сохранённых операций.
    [[nodiscard]] virtual bool next_operation_id(std::string_view& output) noexcept = 0;
};

struct ActionRequest {
    std::string_view operation_id;
    std::string_view correlation_id;
    std::string_view target_module;
    std::string_view action;
    std::optional<std::string_view> parameters;
};

enum class ActionRequestStatus {
    Created,
    InvalidPlan,
    OperationIdUnavailable,
    InvalidOperationId
};

struct ActionRequestResult {
    ActionRequestStatus status{ActionRequestStatus::InvalidPlan};
    ActionRequest request{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == ActionRequestStatus::Created;
    }
};

// Формирует логический запрос и назначает ему ID. Функция не сохраняет запрос,
// не отправляет его исполнителю и не управляет физическим оборудованием.
[[nodiscard]] ActionRequestResult create_action_request(
    const ActionPlan& plan,
    OperationIdProvider& id_provider) noexcept;

}  // namespace dockstation::core
