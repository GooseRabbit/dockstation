#include <cstdlib>
#include <iostream>
#include <string_view>

#include "configuration/configuration_manager.hpp"

using namespace dockstation::configuration;

namespace {

int failures = 0;
int checks = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

void expect_internal_without_snapshot(const ConfigurationSnapshotResult& result,
                                      const char* name) {
    expect(result.status == ConfigurationValidationStatus::InternalError &&
               !result.snapshot.has_value(),
           name);
}

const ConfigurationIssue kWarning{
    "OPTIONAL_FIELD_OMITTED", std::string_view{"station.optional"},
    ConfigurationIssueSeverity::Warning, "Optional field is not configured."};
const ConfigurationIssue kError{
    "REQUIRED_FIELD_MISSING", std::string_view{"station.required"},
    ConfigurationIssueSeverity::Error, "Required field is absent."};
const ConfigurationIssue kUnknownSeverity{
    "TEST_UNKNOWN_SEVERITY", std::nullopt,
    static_cast<ConfigurationIssueSeverity>(255), "Unknown severity."};
const ConfigurationIssue kEmptyReason{
    "", std::nullopt, ConfigurationIssueSeverity::Warning, "Missing reason code."};
const ConfigurationIssue kEmptyDescription{
    "TEST_EMPTY_DESCRIPTION", std::nullopt,
    ConfigurationIssueSeverity::Warning, ""};
const ConfigurationIssue kEmptyPath{
    "TEST_EMPTY_PATH", std::string_view{},
    ConfigurationIssueSeverity::Warning, "Path is malformed."};

ConfigurationCandidate make_candidate() {
    ConfigurationCandidate candidate{};
    candidate.schema_version = "schema-v1";
    candidate.configuration_revision = "revision-42";
    candidate.document.root.type = ConfigurationValueType::Boolean;
    candidate.document.root.value.boolean_value = true;
    return candidate;
}

ConfigurationValidationResult make_valid_result(
    const ConfigurationCandidate& candidate) {
    ConfigurationValidationResult result{};
    result.status = ConfigurationValidationStatus::Valid;
    result.validated_schema_version = candidate.schema_version;
    result.validated_configuration_revision = candidate.configuration_revision;
    return result;
}

}  // namespace

int main() {
    auto candidate = make_candidate();
    auto validation = make_valid_result(candidate);

    auto result = build_validated_configuration_snapshot(candidate, validation);
    expect(result.status == ConfigurationValidationStatus::Valid,
           "VALID validation produces VALID snapshot result");
    expect(result.snapshot.has_value(),
           "successful validation publishes a snapshot");
    expect(result.snapshot->schema_version() == "schema-v1",
           "snapshot preserves the validated schema version");
    expect(result.snapshot->configuration_revision() == "revision-42",
           "snapshot preserves the exact configuration revision");
    expect(result.snapshot->document().root.type == ConfigurationValueType::Boolean &&
               result.snapshot->document().root.value.boolean_value,
           "snapshot preserves the candidate document view");

    const ConfigurationIssue warnings[] = {kWarning};
    validation = make_valid_result(candidate);
    validation.issues = {warnings, 1};
    result = build_validated_configuration_snapshot(candidate, validation);
    expect(result.status == ConfigurationValidationStatus::Valid &&
               result.snapshot.has_value(),
           "WARNING does not block snapshot creation");
    expect(result.issues.items == warnings && result.issues.size == 1 &&
               result.issues.items[0].reason_code == "OPTIONAL_FIELD_OMITTED",
           "snapshot result preserves the original warning view");

    auto invalid = ConfigurationValidationResult{};
    invalid.status = ConfigurationValidationStatus::Invalid;
    invalid.issues = {&kError, 1};
    result = build_validated_configuration_snapshot(candidate, invalid);
    expect(result.status == ConfigurationValidationStatus::Invalid &&
               !result.snapshot.has_value(),
           "INVALID validation never publishes a snapshot");
    expect(result.issues.items == &kError && result.issues.size == 1,
           "failure result preserves its diagnostic issues");

    auto unsupported = ConfigurationValidationResult{};
    unsupported.status = ConfigurationValidationStatus::UnsupportedVersion;
    unsupported.issues = {&kError, 1};
    result = build_validated_configuration_snapshot(candidate, unsupported);
    expect(result.status == ConfigurationValidationStatus::UnsupportedVersion &&
               !result.snapshot.has_value(),
           "UNSUPPORTED_VERSION never publishes a snapshot");

    auto internal = ConfigurationValidationResult{};
    internal.status = ConfigurationValidationStatus::InternalError;
    internal.issues = {&kError, 1};
    result = build_validated_configuration_snapshot(candidate, internal);
    expect(result.status == ConfigurationValidationStatus::InternalError &&
               !result.snapshot.has_value(),
           "INTERNAL_ERROR never publishes a snapshot");

    validation = make_valid_result(candidate);
    validation.validated_schema_version = "another-schema";
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "mismatched validated schema is rejected");

    validation = make_valid_result(candidate);
    validation.validated_configuration_revision = "revision-41";
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "mismatched validated revision is rejected");

    validation = make_valid_result(candidate);
    candidate.configuration_revision = "";
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "empty candidate revision is rejected");
    candidate = make_candidate();
    validation = make_valid_result(candidate);
    candidate.schema_version = "";
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "empty candidate schema version is rejected");

    candidate = make_candidate();
    validation = {};
    validation.status = ConfigurationValidationStatus::Valid;
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "unbound VALID result is rejected");

    validation = make_valid_result(candidate);
    validation.issues = {&kError, 1};
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "VALID result containing ERROR is rejected");

    validation = {};
    validation.status = ConfigurationValidationStatus::Invalid;
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "non-VALID result without an ERROR issue is rejected");

    validation = make_valid_result(candidate);
    validation.issues = {nullptr, 1};
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "issue list with null storage and nonzero size is rejected");

    const ConfigurationIssue* bad_issues[] = {
        &kUnknownSeverity, &kEmptyReason, &kEmptyDescription, &kEmptyPath};
    const char* bad_names[] = {
        "unknown issue severity is rejected",
        "empty issue reason code is rejected",
        "empty issue description is rejected",
        "empty issue path is rejected"};
    for (std::size_t i = 0; i < 4; ++i) {
        validation = make_valid_result(candidate);
        validation.issues = {bad_issues[i], 1};
        expect_internal_without_snapshot(
            build_validated_configuration_snapshot(candidate, validation),
            bad_names[i]);
    }

    validation = make_valid_result(candidate);
    validation.status = static_cast<ConfigurationValidationStatus>(255);
    expect_internal_without_snapshot(
        build_validated_configuration_snapshot(candidate, validation),
        "unknown validation status is rejected");

    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " - " << checks << " checks; failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
