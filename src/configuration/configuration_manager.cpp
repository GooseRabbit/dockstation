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


namespace {

const ConfigurationIssue kInvalidRequiredFieldPolicyIssue{
    "REQUIRED_FIELD_POLICY_INVALID", std::nullopt, ConfigurationIssueSeverity::Error,
    "Required-field policy is empty, malformed or contains duplicate paths."};
const ConfigurationIssue kRequiredFieldIssueBufferTooSmall{
    "REQUIRED_FIELD_ISSUE_BUFFER_TOO_SMALL", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Issue buffer cannot hold every required-field validation issue."};
const ConfigurationIssue kInvalidConfigurationDocumentIssue{
    "CONFIGURATION_DOCUMENT_VIEW_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Configuration document contains an invalid object view."};
const ConfigurationIssue kRequiredFieldMissingTemplate{
    "REQUIRED_FIELD_MISSING", std::nullopt, ConfigurationIssueSeverity::Error,
    "A required configuration field is missing."};
const ConfigurationIssue kRequiredFieldPathUnresolvableTemplate{
    "REQUIRED_FIELD_PATH_UNRESOLVABLE", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "A parent of the required configuration field is not an object."};
const ConfigurationIssue kRequiredFieldPathAmbiguousTemplate{
    "REQUIRED_FIELD_PATH_AMBIGUOUS", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "A required configuration field path is ambiguous because an object contains duplicate names."};

bool paths_equal(const ConfigurationRequiredFieldPathView& left,
                 const ConfigurationRequiredFieldPathView& right) noexcept {
    if (left.size != right.size) return false;
    for (std::size_t i = 0; i < left.size; ++i) {
        if (left.segments[i] != right.segments[i]) return false;
    }
    return true;
}

bool is_valid_required_field_set(
    const ConfigurationRequiredFieldSetView& required_fields) noexcept {
    if (required_fields.fields == nullptr || required_fields.size == 0) return false;
    for (std::size_t i = 0; i < required_fields.size; ++i) {
        const ConfigurationRequiredFieldPathView& field = required_fields.fields[i];
        if (field.segments == nullptr || field.size == 0 || field.diagnostic_path.empty()) {
            return false;
        }
        for (std::size_t segment = 0; segment < field.size; ++segment) {
            if (field.segments[segment].empty()) return false;
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            const ConfigurationRequiredFieldPathView& earlier = required_fields.fields[previous];
            if (field.diagnostic_path == earlier.diagnostic_path || paths_equal(field, earlier)) {
                return false;
            }
        }
    }
    return true;
}

enum class RequiredFieldLookupStatus : std::uint8_t {
    Present, Missing, Unresolvable, Ambiguous, InvalidDocument
};

RequiredFieldLookupStatus lookup_required_field(
    const ConfigurationDocumentView& document,
    const ConfigurationRequiredFieldPathView& path) noexcept {
    ConfigurationValueView current = document.root;
    for (std::size_t segment = 0; segment < path.size; ++segment) {
        if (current.type != ConfigurationValueType::Object) {
            return RequiredFieldLookupStatus::Unresolvable;
        }
        const ConfigurationObjectView object = current.value.object_value;
        if (object.size > 0 && object.entries == nullptr) {
            return RequiredFieldLookupStatus::InvalidDocument;
        }

        const ConfigurationValueView* matched_value = nullptr;
        std::size_t match_count = 0;
        for (std::size_t entry = 0; entry < object.size; ++entry) {
            if (object.entries[entry].name == path.segments[segment]) {
                ++match_count;
                matched_value = &object.entries[entry].value;
            }
        }
        if (match_count == 0) return RequiredFieldLookupStatus::Missing;
        if (match_count > 1) return RequiredFieldLookupStatus::Ambiguous;
        current = *matched_value;
    }
    // Null/Unknown считаются присутствующими; их тип/значение проверяет CFG-F03.
    return RequiredFieldLookupStatus::Present;
}

const ConfigurationIssue& issue_template_for(RequiredFieldLookupStatus status) noexcept {
    switch (status) {
        case RequiredFieldLookupStatus::Missing: return kRequiredFieldMissingTemplate;
        case RequiredFieldLookupStatus::Unresolvable: return kRequiredFieldPathUnresolvableTemplate;
        case RequiredFieldLookupStatus::Ambiguous: return kRequiredFieldPathAmbiguousTemplate;
        case RequiredFieldLookupStatus::Present:
        case RequiredFieldLookupStatus::InvalidDocument: break;
    }
    return kInvalidConfigurationDocumentIssue;
}

}  // namespace

ConfigurationValidationResult validate_required_configuration_fields(
    const ConfigurationCandidate& candidate,
    const ConfigurationRequiredFieldSetView& required_fields,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept {
    if (!is_valid_required_field_set(required_fields)) {
        return make_result(ConfigurationValidationStatus::InternalError,
                           &kInvalidRequiredFieldPolicyIssue);
    }

    // Первый проход определяет объём результата; частичный список не публикуется.
    std::size_t issue_count = 0;
    for (std::size_t i = 0; i < required_fields.size; ++i) {
        const auto lookup = lookup_required_field(candidate.document, required_fields.fields[i]);
        if (lookup == RequiredFieldLookupStatus::InvalidDocument) {
            return make_result(ConfigurationValidationStatus::InternalError,
                               &kInvalidConfigurationDocumentIssue);
        }
        if (lookup != RequiredFieldLookupStatus::Present) ++issue_count;
    }

    if (issue_count == 0) {
        return make_result(ConfigurationValidationStatus::Valid, nullptr);
    }
    if (issue_buffer == nullptr || issue_capacity < issue_count) {
        return make_result(ConfigurationValidationStatus::InternalError,
                           &kRequiredFieldIssueBufferTooSmall);
    }

    // Второй проход заполняет caller-owned буфер проблем.
    std::size_t output_index = 0;
    for (std::size_t i = 0; i < required_fields.size; ++i) {
        const ConfigurationRequiredFieldPathView& path = required_fields.fields[i];
        const auto lookup = lookup_required_field(candidate.document, path);
        if (lookup == RequiredFieldLookupStatus::InvalidDocument) {
            return make_result(ConfigurationValidationStatus::InternalError,
                               &kInvalidConfigurationDocumentIssue);
        }
        if (lookup == RequiredFieldLookupStatus::Present) continue;

        const ConfigurationIssue& issue_template = issue_template_for(lookup);
        issue_buffer[output_index++] = {
            issue_template.reason_code, path.diagnostic_path,
            issue_template.severity, issue_template.description};
    }

    return {ConfigurationValidationStatus::Invalid,
            ConfigurationIssueListView{issue_buffer, output_index}};
}

}  // namespace dockstation::configuration
