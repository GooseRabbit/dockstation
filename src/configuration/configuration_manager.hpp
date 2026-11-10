#pragma once

#include "configuration_types.hpp"

namespace dockstation::configuration {

// Проверяет точное соответствие schema_version одному из явно разрешённых значений.
// Пустой/некорректный набор версий не считается разрешением: возвращается InternalError.
[[nodiscard]] ConfigurationValidationResult validate_configuration_schema_version(
    const ConfigurationCandidate& candidate,
    const ConfigurationSchemaVersionSetView& supported_versions) noexcept;

// Проверяет только наличие явно перечисленных обязательных полей.
// issue_buffer и его память принадлежат вызывающему коду; размер должен быть
// достаточен для всех найденных проблем. При нехватке места возвращается
// InternalError без публикации неполного списка issues.
[[nodiscard]] ConfigurationValidationResult validate_required_configuration_fields(
    const ConfigurationCandidate& candidate,
    const ConfigurationRequiredFieldSetView& required_fields,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept;

[[nodiscard]] ConfigurationValidationResult validate_configuration_values(
    const ConfigurationCandidate& candidate,
    const ConfigurationValueRuleSetView& rules,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept;

// Проверяет прямые отношения между двумя полями по явному набору правил.
[[nodiscard]] ConfigurationValidationResult validate_configuration_consistency(
    const ConfigurationCandidate& candidate,
    const ConfigurationConsistencyRuleSetView& rules,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept;

// Выполняет все обязательные проверки F01-F04 в согласованном порядке и
// возвращает единый список проблем, включая необязательные WARNING. Буфер должен
// вмещать 1 + сумму размеров наборов F02-F04 (один слот для WARNING от F01).
[[nodiscard]] ConfigurationValidationResult validate_configuration(
    const ConfigurationCandidate& candidate,
    const ConfigurationValidationPolicyView& policy,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept;

// Creates a getter-only snapshot only for VALID and exact F05 candidate binding.
[[nodiscard]] ConfigurationSnapshotResult build_validated_configuration_snapshot(
    const ConfigurationCandidate& candidate,
    const ConfigurationValidationResult& validation) noexcept;

class SettingsRepository;
class ConfigurationDecoderPort;

// Reads one persisted record, decodes it, validates the decoded candidate and
// publishes a snapshot only after all validation stages succeed.
[[nodiscard]] ConfigurationLoadResult load_configuration(
    SettingsRepository& repository,
    ConfigurationDecoderPort& decoder,
    const ConfigurationValidationPolicyView& policy,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept;

// Returns the revision only for a caller-designated active, internally
// consistent VALID load result and its validated snapshot. An absent or
// inconsistent active result returns std::nullopt. The returned view refers
// to the snapshot's existing backing storage.
[[nodiscard]] std::optional<std::string_view> get_configuration_revision(
    const ConfigurationLoadResult& active_configuration) noexcept;

}  // namespace dockstation::configuration
