#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

#include "configuration/configuration_manager.hpp"

using namespace dockstation::configuration;

namespace {
const ConfigurationIssue kRequiredIssue{
    "REQUIRED_FIELD_MISSING", std::string_view{"station.required"},
    ConfigurationIssueSeverity::Error, "Required field is missing."};
const ConfigurationIssue kValueIssue{
    "CONFIGURATION_VALUE_NOT_ALLOWED", std::string_view{"station.mode"},
    ConfigurationIssueSeverity::Error, "Value is not allowed."};
const ConfigurationIssue kConsistencyIssue{
    "CONFIGURATION_CONSISTENCY_NOT_SATISFIED",
    std::string_view{"limits.minimum <= limits.maximum"},
    ConfigurationIssueSeverity::Error, "Relation is not satisfied."};
const ConfigurationIssue kSchemaMissing{
    "SCHEMA_VERSION_MISSING", std::nullopt,
    ConfigurationIssueSeverity::Error, "Schema version is missing."};
const ConfigurationIssue kSchemaUnsupported{
    "SCHEMA_VERSION_UNSUPPORTED", std::nullopt,
    ConfigurationIssueSeverity::Error, "Schema version is unsupported."};
const ConfigurationIssue kSchemaPolicyInvalid{
    "SCHEMA_VERSION_POLICY_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error, "Schema version policy is invalid."};
const ConfigurationIssue kValuePolicyInvalid{
    "CONFIGURATION_VALUE_POLICY_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error, "Value policy is invalid."};
const ConfigurationIssue kSchemaWarning{
    "SCHEMA_VERSION_WARNING", std::nullopt,
    ConfigurationIssueSeverity::Warning, "Schema uses a supported optional feature."};
const ConfigurationIssue kValueWarning{
    "CONFIGURATION_VALUE_OPTIONAL_WARNING", std::string_view{"station.optional"},
    ConfigurationIssueSeverity::Warning, "Optional value is not configured."};

int failures = 0;
int checks = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) std::cout << "PASS - " << name << '\n';
    else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

bool has_issue(const ConfigurationValidationResult& result,
               std::string_view reason, std::string_view path) {
    if (result.issues.items == nullptr) return false;
    for (std::size_t i = 0; i < result.issues.size; ++i) {
        const auto& issue = result.issues.items[i];
        if (issue.reason_code == reason && issue.field_path.has_value() &&
            *issue.field_path == path) return true;
    }
    return false;
}

bool has_reason(const ConfigurationValidationResult& result, std::string_view reason) {
    if (result.issues.items == nullptr) return false;
    for (std::size_t i = 0; i < result.issues.size; ++i)
        if (result.issues.items[i].reason_code == reason) return true;
    return false;
}

constexpr std::string_view version_values[] = {"test-v1"};
constexpr std::string_view required_segments[] = {"station", "required"};
constexpr ConfigurationRequiredFieldPathView required_paths[] = {
    {required_segments, 2, "station.required"}};
constexpr std::string_view value_segments[] = {"station", "mode"};
constexpr ConfigurationValueRuleView value_rule[] = {
    {{value_segments, 2, "station.mode"}, ConfigurationValueType::String}};
constexpr std::string_view left_segments[] = {"limits", "minimum"};
constexpr std::string_view right_segments[] = {"limits", "maximum"};
constexpr ConfigurationConsistencyRuleView consistency_rule[] = {
    {{left_segments, 2, "limits.minimum"},
     {right_segments, 2, "limits.maximum"},
     ConfigurationValueType::Number,
     ConfigurationConsistencyOperator::LessThanOrEqual,
     "limits.minimum <= limits.maximum"}};

ConfigurationValidationPolicyView make_policy() {
    return {{version_values, 1},
            {required_paths, 1},
            {value_rule, 1},
            {consistency_rule, 1}};
}

ConfigurationCandidate make_candidate(std::string_view schema = "test-v1",
                                       std::string_view scenario = "valid") {
    ConfigurationCandidate candidate{};
    candidate.schema_version = schema;
    candidate.configuration_revision = scenario;
    return candidate;
}
}  // namespace

// Test doubles isolate the CFG-F05 orchestration contract. The algorithms for
// CFG-F01 through CFG-F04 have their own unit tests.
namespace dockstation::configuration {

ConfigurationValidationResult validate_configuration_schema_version(
    const ConfigurationCandidate& candidate,
    const ConfigurationSchemaVersionSetView& supported_versions) noexcept {
    if (supported_versions.versions == nullptr || supported_versions.size == 0)
        return {ConfigurationValidationStatus::InternalError, {&kSchemaPolicyInvalid, 1}};
    if (candidate.schema_version.empty())
        return {ConfigurationValidationStatus::Invalid, {&kSchemaMissing, 1}};
    for (std::size_t i = 0; i < supported_versions.size; ++i)
        if (candidate.schema_version == supported_versions.versions[i]) {
            if (candidate.configuration_revision == "schema_warning")
                return {ConfigurationValidationStatus::Valid, {&kSchemaWarning, 1}};
            return {ConfigurationValidationStatus::Valid, {}};
        }
    return {ConfigurationValidationStatus::UnsupportedVersion, {&kSchemaUnsupported, 1}};
}

ConfigurationValidationResult validate_required_configuration_fields(
    const ConfigurationCandidate& candidate,
    const ConfigurationRequiredFieldSetView& required_fields,
    ConfigurationIssue* issue_buffer, std::size_t issue_capacity) noexcept {
    if (required_fields.fields == nullptr || required_fields.size == 0)
        return {ConfigurationValidationStatus::InternalError, {&kRequiredIssue, 1}};
    if (candidate.configuration_revision == "all_bad" ||
        candidate.configuration_revision == "internal_plus_invalid") {
        if (issue_buffer == nullptr || issue_capacity < 1)
            return {ConfigurationValidationStatus::InternalError, {&kRequiredIssue, 1}};
        issue_buffer[0] = kRequiredIssue;
        return {ConfigurationValidationStatus::Invalid, {issue_buffer, 1}};
    }
    return {ConfigurationValidationStatus::Valid, {}};
}

ConfigurationValidationResult validate_configuration_values(
    const ConfigurationCandidate& candidate,
    const ConfigurationValueRuleSetView& rules,
    ConfigurationIssue* issue_buffer, std::size_t issue_capacity) noexcept {
    if (rules.rules == nullptr || rules.size == 0 ||
        rules.rules[0].expected_type == ConfigurationValueType::Unknown)
        return {ConfigurationValidationStatus::InternalError, {&kValuePolicyInvalid, 1}};
    if (candidate.configuration_revision == "all_bad") {
        if (issue_buffer == nullptr || issue_capacity < 1)
            return {ConfigurationValidationStatus::InternalError, {&kValuePolicyInvalid, 1}};
        issue_buffer[0] = kValueIssue;
        return {ConfigurationValidationStatus::Invalid, {issue_buffer, 1}};
    }
    if (candidate.configuration_revision == "internal_plus_invalid")
        return {ConfigurationValidationStatus::InternalError, {&kValuePolicyInvalid, 1}};
    if (candidate.configuration_revision == "warning_only") {
        if (issue_buffer == nullptr || issue_capacity < 1)
            return {ConfigurationValidationStatus::InternalError, {&kValuePolicyInvalid, 1}};
        issue_buffer[0] = kValueWarning;
        return {ConfigurationValidationStatus::Valid, {issue_buffer, 1}};
    }
    return {ConfigurationValidationStatus::Valid, {}};
}

ConfigurationValidationResult validate_configuration_consistency(
    const ConfigurationCandidate& candidate,
    const ConfigurationConsistencyRuleSetView& rules,
    ConfigurationIssue* issue_buffer, std::size_t issue_capacity) noexcept {
    if (rules.rules == nullptr || rules.size == 0)
        return {ConfigurationValidationStatus::InternalError, {&kConsistencyIssue, 1}};
    if (candidate.configuration_revision == "all_bad") {
        if (issue_buffer == nullptr || issue_capacity < 1)
            return {ConfigurationValidationStatus::InternalError, {&kConsistencyIssue, 1}};
        issue_buffer[0] = kConsistencyIssue;
        return {ConfigurationValidationStatus::Invalid, {issue_buffer, 1}};
    }
    if (candidate.configuration_revision == "unknown_status")
        return {static_cast<ConfigurationValidationStatus>(255), {}};
    return {ConfigurationValidationStatus::Valid, {}};
}

}  // namespace dockstation::configuration

int main() {
    const auto policy = make_policy();
    ConfigurationIssue issue_buffer[8]{};

    auto result = validate_configuration(make_candidate(), policy, issue_buffer, 4);
    expect(result.status == ConfigurationValidationStatus::Valid,
           "all four validation stages pass");
    expect(result.issues.size == 0 && result.issues.items == nullptr,
           "valid combined result has no issues");

    result = validate_configuration(make_candidate("test-v1", "schema_warning"),
                                    policy, issue_buffer, 4);
    expect(result.status == ConfigurationValidationStatus::Valid &&
           result.issues.size == 1 &&
           result.issues.items[0].reason_code == "SCHEMA_VERSION_WARNING" &&
           result.issues.items[0].severity == ConfigurationIssueSeverity::Warning,
           "non-blocking version warning is retained in a Valid result");

    result = validate_configuration(make_candidate("test-v1", "warning_only"),
                                    policy, issue_buffer, 4);
    expect(result.status == ConfigurationValidationStatus::Valid &&
           result.issues.size == 1 &&
           result.issues.items[0].reason_code == "CONFIGURATION_VALUE_OPTIONAL_WARNING" &&
           result.issues.items[0].severity == ConfigurationIssueSeverity::Warning,
           "non-blocking value warning is retained in a Valid result");

    result = validate_configuration(make_candidate("test-v1", "all_bad"),
                                    policy, issue_buffer, 4);
    expect(result.status == ConfigurationValidationStatus::Invalid,
           "candidate errors produce Invalid status");
    expect(result.issues.size == 3,
           "issues from F02, F03 and F04 are aggregated");
    expect(has_issue(result, "REQUIRED_FIELD_MISSING", "station.required"),
           "required-field issue is retained");
    expect(has_issue(result, "CONFIGURATION_VALUE_NOT_ALLOWED", "station.mode"),
           "value-validation issue is retained");
    expect(has_issue(result, "CONFIGURATION_CONSISTENCY_NOT_SATISFIED",
                     "limits.minimum <= limits.maximum"),
           "consistency issue is retained");
    expect(result.issues.items == issue_buffer,
           "combined issues use the caller-owned buffer");
    expect(result.issues.items[0].reason_code == "REQUIRED_FIELD_MISSING" &&
           result.issues.items[1].reason_code == "CONFIGURATION_VALUE_NOT_ALLOWED" &&
           result.issues.items[2].reason_code == "CONFIGURATION_CONSISTENCY_NOT_SATISFIED",
           "aggregated issues preserve validation order F02, F03, F04");

    issue_buffer[0] = {"SENTINEL", std::nullopt,
                       ConfigurationIssueSeverity::Warning, "untouched"};
    result = validate_configuration(make_candidate("unknown-v9"), policy,
                                    issue_buffer, 3);
    expect(result.status == ConfigurationValidationStatus::UnsupportedVersion,
           "unsupported schema version short-circuits later checks");
    expect(has_reason(result, "SCHEMA_VERSION_UNSUPPORTED"),
           "schema-version issue is returned");
    expect(issue_buffer[0].reason_code == "SENTINEL",
           "schema-version short circuit does not modify aggregation buffer");

    result = validate_configuration(make_candidate(""), policy, issue_buffer, 4);
    expect(result.status == ConfigurationValidationStatus::Invalid &&
           has_reason(result, "SCHEMA_VERSION_MISSING"),
           "missing schema version is returned as Invalid");

    result = validate_configuration(make_candidate(), policy, issue_buffer, 2);
    expect(result.status == ConfigurationValidationStatus::InternalError &&
           has_reason(result, "CONFIGURATION_VALIDATION_ISSUE_BUFFER_TOO_SMALL"),
           "worst-case output capacity is checked before validators run");
    expect(issue_buffer[0].reason_code == "SENTINEL",
           "undersized output buffer is not partially overwritten");
    result = validate_configuration(make_candidate(), policy, nullptr, 0);
    expect(result.status == ConfigurationValidationStatus::InternalError,
           "null output buffer fails closed");

    auto invalid_shape = policy;
    invalid_shape.required_fields = {nullptr, 1};
    result = validate_configuration(make_candidate(), invalid_shape, issue_buffer, 8);
    expect(result.status == ConfigurationValidationStatus::InternalError &&
           has_reason(result, "CONFIGURATION_VALIDATION_POLICY_INVALID"),
           "missing required-field policy array is rejected");

    const ConfigurationValueRuleView invalid_rule[] = {
        {{value_segments, 2, "station.mode"}, ConfigurationValueType::Unknown}};
    auto invalid_value_policy = policy;
    invalid_value_policy.value_rules = {invalid_rule, 1};
    result = validate_configuration(make_candidate("test-v1", "internal_plus_invalid"),
                                    invalid_value_policy, issue_buffer, 4);
    expect(result.status == ConfigurationValidationStatus::InternalError,
           "internal error takes precedence over candidate Invalid");
    expect(result.issues.size == 2 &&
           has_issue(result, "REQUIRED_FIELD_MISSING", "station.required") &&
           has_reason(result, "CONFIGURATION_VALUE_POLICY_INVALID"),
           "candidate and internal-policy issues are both retained");

    auto overflow_policy = policy;
    overflow_policy.required_fields.size = std::numeric_limits<std::size_t>::max();
    issue_buffer[0] = {"SENTINEL", std::nullopt,
                       ConfigurationIssueSeverity::Warning, "untouched"};
    result = validate_configuration(make_candidate(), overflow_policy, issue_buffer, 8);
    expect(result.status == ConfigurationValidationStatus::InternalError &&
           has_reason(result, "CONFIGURATION_VALIDATION_POLICY_INVALID"),
           "worst-case issue-count overflow fails closed");
    expect(issue_buffer[0].reason_code == "SENTINEL",
           "issue-count overflow is rejected before buffer modification");

    result = validate_configuration(make_candidate("test-v1", "unknown_status"),
                                    policy, issue_buffer, 4);
    expect(result.status == ConfigurationValidationStatus::InternalError &&
           has_reason(result, "CONFIGURATION_VALIDATION_SUBSTATUS_UNKNOWN"),
           "unknown subordinate status fails closed");

    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " - " << checks << " checks; failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
