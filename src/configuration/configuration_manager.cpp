#include "configuration_manager.hpp"

#include <cmath>

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


namespace {
const ConfigurationIssue kInvalidValueRulePolicyIssue{
    "CONFIGURATION_VALUE_POLICY_INVALID", std::nullopt, ConfigurationIssueSeverity::Error,
    "Configuration value policy is empty, malformed or contains duplicate rules."};
const ConfigurationIssue kValueIssueBufferTooSmall{
    "CONFIGURATION_VALUE_ISSUE_BUFFER_TOO_SMALL", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Issue buffer cannot hold every configuration value validation issue."};

bool is_numeric_type(ConfigurationValueType type) noexcept {
    return type == ConfigurationValueType::SignedInteger ||
           type == ConfigurationValueType::UnsignedInteger ||
           type == ConfigurationValueType::Number;
}
bool is_scalar_type(ConfigurationValueType type) noexcept {
    return type == ConfigurationValueType::Null || type == ConfigurationValueType::Boolean ||
           type == ConfigurationValueType::SignedInteger ||
           type == ConfigurationValueType::UnsignedInteger ||
           type == ConfigurationValueType::Number || type == ConfigurationValueType::String;
}
bool is_known_value_type(ConfigurationValueType type) noexcept {
    switch (type) {
        case ConfigurationValueType::Null:
        case ConfigurationValueType::Boolean:
        case ConfigurationValueType::SignedInteger:
        case ConfigurationValueType::UnsignedInteger:
        case ConfigurationValueType::Number:
        case ConfigurationValueType::String:
        case ConfigurationValueType::Object:
        case ConfigurationValueType::Array:
        case ConfigurationValueType::Opaque: return true;
        case ConfigurationValueType::Unknown: return false;
    }
    return false;
}
bool scalar_values_equal(const ConfigurationValueView& a,
                         const ConfigurationValueView& b) noexcept {
    if (a.type != b.type) return false;
    switch (a.type) {
        case ConfigurationValueType::Null: return true;
        case ConfigurationValueType::Boolean: return a.value.boolean_value == b.value.boolean_value;
        case ConfigurationValueType::SignedInteger:
            return a.value.signed_integer_value == b.value.signed_integer_value;
        case ConfigurationValueType::UnsignedInteger:
            return a.value.unsigned_integer_value == b.value.unsigned_integer_value;
        case ConfigurationValueType::Number: return a.value.number_value == b.value.number_value;
        case ConfigurationValueType::String: return a.value.string_value == b.value.string_value;
        case ConfigurationValueType::Unknown:
        case ConfigurationValueType::Object:
        case ConfigurationValueType::Array:
        case ConfigurationValueType::Opaque: return false;
    }
    return false;
}
bool numeric_less_or_equal(const ConfigurationValueView& a,
                           const ConfigurationValueView& b) noexcept {
    if (a.type != b.type) return false;
    switch (a.type) {
        case ConfigurationValueType::SignedInteger:
            return a.value.signed_integer_value <= b.value.signed_integer_value;
        case ConfigurationValueType::UnsignedInteger:
            return a.value.unsigned_integer_value <= b.value.unsigned_integer_value;
        case ConfigurationValueType::Number: return a.value.number_value <= b.value.number_value;
        default: return false;
    }
}
bool valid_numeric_bound(const ConfigurationValueView& value,
                         ConfigurationValueType expected) noexcept {
    return value.type == expected && is_numeric_type(expected) &&
           (expected != ConfigurationValueType::Number || std::isfinite(value.value.number_value));
}
bool valid_value_rule(const ConfigurationValueRuleView& rule) noexcept {
    if (rule.path.segments == nullptr || rule.path.size == 0 ||
        rule.path.diagnostic_path.empty() || !is_known_value_type(rule.expected_type)) return false;
    for (std::size_t i = 0; i < rule.path.size; ++i)
        if (rule.path.segments[i].empty()) return false;
    if ((rule.has_minimum && !valid_numeric_bound(rule.minimum, rule.expected_type)) ||
        (rule.has_maximum && !valid_numeric_bound(rule.maximum, rule.expected_type))) return false;
    if (rule.has_minimum && rule.has_maximum &&
        !numeric_less_or_equal(rule.minimum, rule.maximum)) return false;
    if (rule.allowed_values_size > 0 && rule.allowed_values == nullptr) return false;
    if (rule.allowed_values_size > 0 && !is_scalar_type(rule.expected_type)) return false;
    for (std::size_t i = 0; i < rule.allowed_values_size; ++i) {
        const auto& value = rule.allowed_values[i];
        if (value.type != rule.expected_type || !is_scalar_type(value.type) ||
            (value.type == ConfigurationValueType::Number && !std::isfinite(value.value.number_value)))
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (scalar_values_equal(value, rule.allowed_values[j])) return false;
    }
    return true;
}
bool valid_value_rules(const ConfigurationValueRuleSetView& rules) noexcept {
    if (rules.rules == nullptr || rules.size == 0) return false;
    for (std::size_t i = 0; i < rules.size; ++i) {
        if (!valid_value_rule(rules.rules[i])) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (rules.rules[i].path.diagnostic_path == rules.rules[j].path.diagnostic_path ||
                paths_equal(rules.rules[i].path, rules.rules[j].path)) return false;
    }
    return true;
}
enum class ValueIssue : std::uint8_t {
    None, Missing, Unresolvable, Ambiguous, TypeMismatch, Below, Above, NotAllowed, NotFinite,
    InvalidDocument
};
ValueIssue check_value(const ConfigurationDocumentView& document,
                       const ConfigurationValueRuleView& rule) noexcept {
    const auto lookup = lookup_required_field(document, rule.path);
    switch (lookup) {
        case RequiredFieldLookupStatus::Missing: return ValueIssue::Missing;
        case RequiredFieldLookupStatus::Unresolvable: return ValueIssue::Unresolvable;
        case RequiredFieldLookupStatus::Ambiguous: return ValueIssue::Ambiguous;
        case RequiredFieldLookupStatus::InvalidDocument: return ValueIssue::InvalidDocument;
        case RequiredFieldLookupStatus::Present: break;
    }
    ConfigurationValueView value = document.root;
    for (std::size_t i = 0; i < rule.path.size; ++i) {
        const auto object = value.value.object_value;
        for (std::size_t j = 0; j < object.size; ++j)
            if (object.entries[j].name == rule.path.segments[i]) {
                value = object.entries[j].value;
                break;
            }
    }
    if (value.type != rule.expected_type) return ValueIssue::TypeMismatch;
    if (value.type == ConfigurationValueType::Number && !std::isfinite(value.value.number_value))
        return ValueIssue::NotFinite;
    if (rule.has_minimum && !numeric_less_or_equal(rule.minimum, value)) return ValueIssue::Below;
    if (rule.has_maximum && !numeric_less_or_equal(value, rule.maximum)) return ValueIssue::Above;
    if (rule.allowed_values_size > 0) {
        for (std::size_t i = 0; i < rule.allowed_values_size; ++i)
            if (scalar_values_equal(value, rule.allowed_values[i])) return ValueIssue::None;
        return ValueIssue::NotAllowed;
    }
    return ValueIssue::None;
}
const ConfigurationIssue& issue_for(ValueIssue issue) noexcept {
    static const ConfigurationIssue missing{"CONFIGURATION_VALUE_FIELD_MISSING", std::nullopt,
        ConfigurationIssueSeverity::Error, "A field required by the value policy is missing."};
    static const ConfigurationIssue unresolvable{"CONFIGURATION_VALUE_PATH_UNRESOLVABLE", std::nullopt,
        ConfigurationIssueSeverity::Error, "A parent of the configured value path is not an object."};
    static const ConfigurationIssue ambiguous{"CONFIGURATION_VALUE_PATH_AMBIGUOUS", std::nullopt,
        ConfigurationIssueSeverity::Error, "The configured value path contains duplicate object keys."};
    static const ConfigurationIssue mismatch{"CONFIGURATION_VALUE_TYPE_MISMATCH", std::nullopt,
        ConfigurationIssueSeverity::Error, "The configuration value has a different type than required."};
    static const ConfigurationIssue below{"CONFIGURATION_VALUE_BELOW_MINIMUM", std::nullopt,
        ConfigurationIssueSeverity::Error, "The configuration value is below the configured minimum."};
    static const ConfigurationIssue above{"CONFIGURATION_VALUE_ABOVE_MAXIMUM", std::nullopt,
        ConfigurationIssueSeverity::Error, "The configuration value is above the configured maximum."};
    static const ConfigurationIssue not_allowed{"CONFIGURATION_VALUE_NOT_ALLOWED", std::nullopt,
        ConfigurationIssueSeverity::Error, "The configuration value is not in the explicitly allowed set."};
    static const ConfigurationIssue not_finite{"CONFIGURATION_VALUE_NOT_FINITE", std::nullopt,
        ConfigurationIssueSeverity::Error, "A numeric configuration value must be finite."};
    switch (issue) {
        case ValueIssue::Missing: return missing;
        case ValueIssue::Unresolvable: return unresolvable;
        case ValueIssue::Ambiguous: return ambiguous;
        case ValueIssue::TypeMismatch: return mismatch;
        case ValueIssue::Below: return below;
        case ValueIssue::Above: return above;
        case ValueIssue::NotAllowed: return not_allowed;
        case ValueIssue::NotFinite: return not_finite;
        case ValueIssue::None:
        case ValueIssue::InvalidDocument: break;
    }
    return kInvalidConfigurationDocumentIssue;
}
}  // namespace

ConfigurationValidationResult validate_configuration_values(
    const ConfigurationCandidate& candidate, const ConfigurationValueRuleSetView& rules,
    ConfigurationIssue* issue_buffer, std::size_t issue_capacity) noexcept {
    if (!valid_value_rules(rules))
        return make_result(ConfigurationValidationStatus::InternalError, &kInvalidValueRulePolicyIssue);
    std::size_t count = 0;
    for (std::size_t i = 0; i < rules.size; ++i) {
        const auto issue = check_value(candidate.document, rules.rules[i]);
        if (issue == ValueIssue::InvalidDocument)
            return make_result(ConfigurationValidationStatus::InternalError,
                               &kInvalidConfigurationDocumentIssue);
        if (issue != ValueIssue::None) ++count;
    }
    if (count == 0) return make_result(ConfigurationValidationStatus::Valid, nullptr);
    if (issue_buffer == nullptr || issue_capacity < count)
        return make_result(ConfigurationValidationStatus::InternalError, &kValueIssueBufferTooSmall);
    std::size_t output = 0;
    for (std::size_t i = 0; i < rules.size; ++i) {
        const auto& rule = rules.rules[i];
        const auto issue = check_value(candidate.document, rule);
        if (issue == ValueIssue::None) continue;
        const auto& item = issue_for(issue);
        issue_buffer[output++] = {item.reason_code, rule.path.diagnostic_path,
                                  item.severity, item.description};
    }
    return {ConfigurationValidationStatus::Invalid, {issue_buffer, output}};
}

}  // namespace dockstation::configuration
