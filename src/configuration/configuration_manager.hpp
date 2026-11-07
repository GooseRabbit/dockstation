#pragma once

#include "configuration_types.hpp"

namespace dockstation::configuration {

// Проверяет точное соответствие schema_version одному из явно разрешённых значений.
// Пустой/некорректный набор версий не считается разрешением: возвращается InternalError.
[[nodiscard]] ConfigurationValidationResult validate_configuration_schema_version(
    const ConfigurationCandidate& candidate,
    const ConfigurationSchemaVersionSetView& supported_versions) noexcept;

}  // namespace dockstation::configuration
