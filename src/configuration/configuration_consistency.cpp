#include "configuration_manager.hpp"

#include <cmath>

namespace dockstation::configuration {
namespace {
const ConfigurationIssue kInvalidPolicyIssue{
    "CONFIGURATION_CONSISTENCY_POLICY_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Consistency policy is empty, malformed or contains duplicate rules."};
const ConfigurationIssue kInvalidDocumentIssue{
    "CONFIGURATION_CONSISTENCY_DOCUMENT_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Configuration document contains an invalid object view."};
const ConfigurationIssue kIssueBufferTooSmall{
    "CONFIGURATION_CONSISTENCY_ISSUE_BUFFER_TOO_SMALL", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Issue buffer cannot hold every consistency validation issue."};

ConfigurationValidationResult make_result(ConfigurationValidationStatus status,
                                           const ConfigurationIssue* issue) noexcept {
    return {status, issue == nullptr ? ConfigurationIssueListView{}
                                    : ConfigurationIssueListView{issue, 1}};
}
bool is_numeric_type(ConfigurationValueType type) noexcept {
    return type == ConfigurationValueType::SignedInteger ||
           type == ConfigurationValueType::UnsignedInteger ||
           type == ConfigurationValueType::Number;
}
bool is_scalar_type(ConfigurationValueType type) noexcept {
    return type == ConfigurationValueType::Null ||
           type == ConfigurationValueType::Boolean || is_numeric_type(type) ||
           type == ConfigurationValueType::String;
}
bool is_known_operator(ConfigurationConsistencyOperator operation) noexcept {
    switch (operation) {
        case ConfigurationConsistencyOperator::Equal:
        case ConfigurationConsistencyOperator::NotEqual:
        case ConfigurationConsistencyOperator::LessThan:
        case ConfigurationConsistencyOperator::LessThanOrEqual:
        case ConfigurationConsistencyOperator::GreaterThan:
        case ConfigurationConsistencyOperator::GreaterThanOrEqual: return true;
    }
    return false;
}
bool is_order_operator(ConfigurationConsistencyOperator operation) noexcept {
    return operation == ConfigurationConsistencyOperator::LessThan ||
           operation == ConfigurationConsistencyOperator::LessThanOrEqual ||
           operation == ConfigurationConsistencyOperator::GreaterThan ||
           operation == ConfigurationConsistencyOperator::GreaterThanOrEqual;
}
bool paths_equal(const ConfigurationRequiredFieldPathView& left,
                 const ConfigurationRequiredFieldPathView& right) noexcept {
    if (left.size != right.size) return false;
    for (std::size_t i = 0; i < left.size; ++i)
        if (left.segments[i] != right.segments[i]) return false;
    return true;
}
bool valid_path(const ConfigurationRequiredFieldPathView& path) noexcept {
    if (!path.segments || path.size == 0 || path.diagnostic_path.empty()) return false;
    for (std::size_t i = 0; i < path.size; ++i)
        if (path.segments[i].empty()) return false;
    return true;
}
bool valid_rule(const ConfigurationConsistencyRuleView& rule) noexcept {
    if (!valid_path(rule.left_path) || !valid_path(rule.right_path) ||
        rule.diagnostic_path.empty() || !is_known_operator(rule.operation) ||
        !is_scalar_type(rule.expected_type) || paths_equal(rule.left_path, rule.right_path))
        return false;
    return !is_order_operator(rule.operation) || is_numeric_type(rule.expected_type);
}
bool valid_rule_set(const ConfigurationConsistencyRuleSetView& rules) noexcept {
    if (!rules.rules || rules.size == 0) return false;
    for (std::size_t i = 0; i < rules.size; ++i) {
        const auto& rule = rules.rules[i];
        if (!valid_rule(rule)) return false;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& earlier = rules.rules[j];
            if (rule.diagnostic_path == earlier.diagnostic_path ||
                (rule.operation == earlier.operation &&
                 paths_equal(rule.left_path, earlier.left_path) &&
                 paths_equal(rule.right_path, earlier.right_path))) return false;
        }
    }
    return true;
}
enum class LookupStatus : std::uint8_t { Found, Missing, Unresolvable, Ambiguous, InvalidDocument };
struct LookupResult {
    LookupStatus status{LookupStatus::InvalidDocument};
    const ConfigurationValueView* value{nullptr};
};
LookupResult lookup_path(const ConfigurationDocumentView& document,
                         const ConfigurationRequiredFieldPathView& path) noexcept {
    const ConfigurationValueView* current = &document.root;
    for (std::size_t segment = 0; segment < path.size; ++segment) {
        if (current->type != ConfigurationValueType::Object)
            return {LookupStatus::Unresolvable, nullptr};
        const ConfigurationObjectView object = current->value.object_value;
        if (object.size > 0 && !object.entries)
            return {LookupStatus::InvalidDocument, nullptr};
        const ConfigurationValueView* match = nullptr;
        std::size_t matches = 0;
        for (std::size_t i = 0; i < object.size; ++i) {
            if (object.entries[i].name == path.segments[segment]) {
                match = &object.entries[i].value;
                ++matches;
            }
        }
        if (matches == 0) return {LookupStatus::Missing, nullptr};
        if (matches > 1) return {LookupStatus::Ambiguous, nullptr};
        current = match;
    }
    return {LookupStatus::Found, current};
}
enum class RuleIssue : std::uint8_t {
    None, LeftMissing, RightMissing, LeftUnresolvable, RightUnresolvable,
    LeftAmbiguous, RightAmbiguous, TypeMismatch, ValueNotFinite, Inconsistent,
    InvalidDocument
};
bool scalar_equal(const ConfigurationValueView& left,
                  const ConfigurationValueView& right) noexcept {
    switch (left.type) {
        case ConfigurationValueType::Null: return true;
        case ConfigurationValueType::Boolean: return left.value.boolean_value == right.value.boolean_value;
        case ConfigurationValueType::SignedInteger: return left.value.signed_integer_value == right.value.signed_integer_value;
        case ConfigurationValueType::UnsignedInteger: return left.value.unsigned_integer_value == right.value.unsigned_integer_value;
        case ConfigurationValueType::Number: return left.value.number_value == right.value.number_value;
        case ConfigurationValueType::String: return left.value.string_value == right.value.string_value;
        case ConfigurationValueType::Unknown:
        case ConfigurationValueType::Object:
        case ConfigurationValueType::Array:
        case ConfigurationValueType::Opaque: return false;
    }
    return false;
}
int compare_numeric(const ConfigurationValueView& left,
                    const ConfigurationValueView& right) noexcept {
    switch (left.type) {
        case ConfigurationValueType::SignedInteger:
            return left.value.signed_integer_value < right.value.signed_integer_value ? -1
                 : left.value.signed_integer_value > right.value.signed_integer_value ? 1 : 0;
        case ConfigurationValueType::UnsignedInteger:
            return left.value.unsigned_integer_value < right.value.unsigned_integer_value ? -1
                 : left.value.unsigned_integer_value > right.value.unsigned_integer_value ? 1 : 0;
        case ConfigurationValueType::Number:
            return left.value.number_value < right.value.number_value ? -1
                 : left.value.number_value > right.value.number_value ? 1 : 0;
        case ConfigurationValueType::Unknown:
        case ConfigurationValueType::Null:
        case ConfigurationValueType::Boolean:
        case ConfigurationValueType::String:
        case ConfigurationValueType::Object:
        case ConfigurationValueType::Array:
        case ConfigurationValueType::Opaque: return 0;
    }
    return 0;
}
RuleIssue evaluate_rule(const ConfigurationDocumentView& document,
                        const ConfigurationConsistencyRuleView& rule) noexcept {
    const LookupResult left = lookup_path(document, rule.left_path);
    const LookupResult right = lookup_path(document, rule.right_path);
    if (left.status == LookupStatus::InvalidDocument ||
        right.status == LookupStatus::InvalidDocument) return RuleIssue::InvalidDocument;
    if (left.status == LookupStatus::Missing) return RuleIssue::LeftMissing;
    if (right.status == LookupStatus::Missing) return RuleIssue::RightMissing;
    if (left.status == LookupStatus::Unresolvable) return RuleIssue::LeftUnresolvable;
    if (right.status == LookupStatus::Unresolvable) return RuleIssue::RightUnresolvable;
    if (left.status == LookupStatus::Ambiguous) return RuleIssue::LeftAmbiguous;
    if (right.status == LookupStatus::Ambiguous) return RuleIssue::RightAmbiguous;
    if (!left.value || !right.value) return RuleIssue::InvalidDocument;
    if (left.value->type != rule.expected_type || right.value->type != rule.expected_type)
        return RuleIssue::TypeMismatch;
    if (rule.expected_type == ConfigurationValueType::Number &&
        (!std::isfinite(left.value->value.number_value) ||
         !std::isfinite(right.value->value.number_value))) return RuleIssue::ValueNotFinite;

    bool satisfied = false;
    if (rule.operation == ConfigurationConsistencyOperator::Equal ||
        rule.operation == ConfigurationConsistencyOperator::NotEqual) {
        const bool equal = scalar_equal(*left.value, *right.value);
        satisfied = rule.operation == ConfigurationConsistencyOperator::Equal ? equal : !equal;
    } else {
        const int comparison = compare_numeric(*left.value, *right.value);
        switch (rule.operation) {
            case ConfigurationConsistencyOperator::LessThan: satisfied = comparison < 0; break;
            case ConfigurationConsistencyOperator::LessThanOrEqual: satisfied = comparison <= 0; break;
            case ConfigurationConsistencyOperator::GreaterThan: satisfied = comparison > 0; break;
            case ConfigurationConsistencyOperator::GreaterThanOrEqual: satisfied = comparison >= 0; break;
            case ConfigurationConsistencyOperator::Equal:
            case ConfigurationConsistencyOperator::NotEqual: break;
        }
    }
    return satisfied ? RuleIssue::None : RuleIssue::Inconsistent;
}
const ConfigurationIssue& issue_template(RuleIssue issue) noexcept {
    static const ConfigurationIssue left_missing{
        "CONFIGURATION_CONSISTENCY_LEFT_FIELD_MISSING", std::nullopt, ConfigurationIssueSeverity::Error,
        "The left field required by a consistency rule is missing."};
    static const ConfigurationIssue right_missing{
        "CONFIGURATION_CONSISTENCY_RIGHT_FIELD_MISSING", std::nullopt, ConfigurationIssueSeverity::Error,
        "The right field required by a consistency rule is missing."};
    static const ConfigurationIssue left_unresolvable{
        "CONFIGURATION_CONSISTENCY_LEFT_PATH_UNRESOLVABLE", std::nullopt, ConfigurationIssueSeverity::Error,
        "A parent of the left consistency path is not an object."};
    static const ConfigurationIssue right_unresolvable{
        "CONFIGURATION_CONSISTENCY_RIGHT_PATH_UNRESOLVABLE", std::nullopt, ConfigurationIssueSeverity::Error,
        "A parent of the right consistency path is not an object."};
    static const ConfigurationIssue left_ambiguous{
        "CONFIGURATION_CONSISTENCY_LEFT_PATH_AMBIGUOUS", std::nullopt, ConfigurationIssueSeverity::Error,
        "The left consistency path contains duplicate object keys."};
    static const ConfigurationIssue right_ambiguous{
        "CONFIGURATION_CONSISTENCY_RIGHT_PATH_AMBIGUOUS", std::nullopt, ConfigurationIssueSeverity::Error,
        "The right consistency path contains duplicate object keys."};
    static const ConfigurationIssue type_mismatch{
        "CONFIGURATION_CONSISTENCY_TYPE_MISMATCH", std::nullopt, ConfigurationIssueSeverity::Error,
        "A field has a different type than the consistency rule requires."};
    static const ConfigurationIssue not_finite{
        "CONFIGURATION_CONSISTENCY_VALUE_NOT_FINITE", std::nullopt, ConfigurationIssueSeverity::Error,
        "Numeric values used in consistency checks must be finite."};
    static const ConfigurationIssue inconsistent{
        "CONFIGURATION_CONSISTENCY_NOT_SATISFIED", std::nullopt, ConfigurationIssueSeverity::Error,
        "The configuration fields do not satisfy their declared consistency rule."};
    switch (issue) {
        case RuleIssue::LeftMissing: return left_missing;
        case RuleIssue::RightMissing: return right_missing;
        case RuleIssue::LeftUnresolvable: return left_unresolvable;
        case RuleIssue::RightUnresolvable: return right_unresolvable;
        case RuleIssue::LeftAmbiguous: return left_ambiguous;
        case RuleIssue::RightAmbiguous: return right_ambiguous;
        case RuleIssue::TypeMismatch: return type_mismatch;
        case RuleIssue::ValueNotFinite: return not_finite;
        case RuleIssue::Inconsistent: return inconsistent;
        case RuleIssue::None:
        case RuleIssue::InvalidDocument: break;
    }
    return kInvalidDocumentIssue;
}
} // namespace

ConfigurationValidationResult validate_configuration_consistency(
    const ConfigurationCandidate& candidate,
    const ConfigurationConsistencyRuleSetView& rules,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept {
    if (!valid_rule_set(rules))
        return make_result(ConfigurationValidationStatus::InternalError, &kInvalidPolicyIssue);
    std::size_t issue_count = 0;
    for (std::size_t i = 0; i < rules.size; ++i) {
        const RuleIssue issue = evaluate_rule(candidate.document, rules.rules[i]);
        if (issue == RuleIssue::InvalidDocument)
            return make_result(ConfigurationValidationStatus::InternalError, &kInvalidDocumentIssue);
        if (issue != RuleIssue::None) ++issue_count;
    }
    if (issue_count == 0) return make_result(ConfigurationValidationStatus::Valid, nullptr);
    if (!issue_buffer || issue_capacity < issue_count)
        return make_result(ConfigurationValidationStatus::InternalError, &kIssueBufferTooSmall);

    std::size_t output = 0;
    for (std::size_t i = 0; i < rules.size; ++i) {
        const auto& rule = rules.rules[i];
        const RuleIssue issue = evaluate_rule(candidate.document, rule);
        if (issue == RuleIssue::None) continue;
        const auto& item = issue_template(issue);
        issue_buffer[output++] = {item.reason_code, rule.diagnostic_path, item.severity, item.description};
    }
    return {ConfigurationValidationStatus::Invalid, {issue_buffer, output}};
}
} // namespace dockstation::configuration
