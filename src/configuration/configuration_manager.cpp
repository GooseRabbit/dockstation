#include "configuration_manager.hpp"

namespace dockstation::configuration {
namespace {

const ConfigurationIssue kMissingVersionIssue{
    "SCHEMA_VERSION_MISSING", std::nullopt, ConfigurationIssueSeverity::Error,
    "Schema version is required."};

const ConfigurationIssue kMalformedVersionIssue{
    "SCHEMA_VERSION_MALFORMED", std::nullopt, ConfigurationIssueSeverity::Error,
    "Schema version must not contain ASCII whitespace, control characters or NUL."};

const ConfigurationIssue kUnsupportedVersionIssue{
    "SCHEMA_VERSION_UNSUPPORTED", std::nullopt, ConfigurationIssueSeverity::Error,
    "Schema version is not present in the explicitly supported set."};

const ConfigurationIssue kInvalidPolicyIssue{
    "SCHEMA_VERSION_POLICY_INVALID", std::nullopt, ConfigurationIssueSeverity::Error,
    "Supported schema version policy is empty or malformed."};

// Версия — непрозрачный идентификатор. SemVer/числовой формат не навязывается;
// исключаются только пустые строки, ASCII whitespace, control и NUL.
bool is_valid_schema_version_token(std::string_view version) noexcept {
    if (version.empty()) {
        return false;
    }
    for (const char raw_character : version) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character <= 0x20U || character == 0x7FU) {
            return false;
        }
    }
    return true;
}

// Политика должна содержать хотя бы одну корректную и уникальную версию.
bool is_valid_supported_version_set(
    const ConfigurationSchemaVersionSetView& supported_versions) noexcept {
    if (supported_versions.versions == nullptr || supported_versions.size == 0) {
        return false;
    }
    for (std::size_t i = 0; i < supported_versions.size; ++i) {
        const std::string_view version = supported_versions.versions[i];
        if (!is_valid_schema_version_token(version)) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (version == supported_versions.versions[j]) {
                return false;
            }
        }
    }
    return true;
}

ConfigurationValidationResult make_result(
    ConfigurationValidationStatus status,
    const ConfigurationIssue* issue) noexcept {
    return {status, issue == nullptr ? ConfigurationIssueListView{}
                                     : ConfigurationIssueListView{issue, 1}};
}

}  // namespace

ConfigurationValidationResult validate_configuration_schema_version(
    const ConfigurationCandidate& candidate,
    const ConfigurationSchemaVersionSetView& supported_versions) noexcept {
    // Пустая/повреждённая политика — ошибка настройки проверки, а не данных.
    if (!is_valid_supported_version_set(supported_versions)) {
        return make_result(ConfigurationValidationStatus::InternalError,
                           &kInvalidPolicyIssue);
    }
    if (candidate.schema_version.empty()) {
        return make_result(ConfigurationValidationStatus::Invalid,
                           &kMissingVersionIssue);
    }
    if (!is_valid_schema_version_token(candidate.schema_version)) {
        return make_result(ConfigurationValidationStatus::Invalid,
                           &kMalformedVersionIssue);
    }

    // Сравнение точное: без нормализации регистра, пробелов или формата версии.
    for (std::size_t i = 0; i < supported_versions.size; ++i) {
        if (candidate.schema_version == supported_versions.versions[i]) {
            return make_result(ConfigurationValidationStatus::Valid, nullptr);
        }
    }
    return make_result(ConfigurationValidationStatus::UnsupportedVersion,
                       &kUnsupportedVersionIssue);
}

}  // namespace dockstation::configuration
