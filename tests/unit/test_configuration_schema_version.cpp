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

bool has_issue(const ConfigurationValidationResult& result,
               std::string_view reason_code) {
    if (result.issues.size == 0 || result.issues.items == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < result.issues.size; ++i) {
        if (result.issues.items[i].reason_code == reason_code) {
            return true;
        }
    }
    return false;
}
}  // namespace

int main() {
    // Тестовые идентификаторы не являются утверждёнными версиями DockStation.
    constexpr std::string_view test_supported_versions[] = {"test-v1", "test-v2"};
    const ConfigurationSchemaVersionSetView supported{test_supported_versions, 2};

    ConfigurationCandidate candidate{};
    candidate.schema_version = "test-v1";

    const ConfigurationValidationResult default_result{};
    expect(default_result.status == ConfigurationValidationStatus::InternalError,
           "default validation result fails closed");
    expect(default_result.issues.size == 0 && default_result.issues.items == nullptr,
           "default validation result has no dangling issue storage");

    const auto supported_result =
        validate_configuration_schema_version(candidate, supported);
    expect(supported_result.status == ConfigurationValidationStatus::Valid,
           "explicitly supported schema version is accepted");
    expect(supported_result.issues.size == 0 && supported_result.issues.items == nullptr,
           "valid schema version has no issues");

    candidate.schema_version = "test-v3";
    const auto unknown_result =
        validate_configuration_schema_version(candidate, supported);
    expect(unknown_result.status == ConfigurationValidationStatus::UnsupportedVersion,
           "well-formed but unknown schema version is unsupported");
    expect(has_issue(unknown_result, "SCHEMA_VERSION_UNSUPPORTED"),
           "unsupported version has a stable reason code");

    candidate.schema_version = "TEST-v1";
    const auto case_mismatch_result =
        validate_configuration_schema_version(candidate, supported);
    expect(case_mismatch_result.status == ConfigurationValidationStatus::UnsupportedVersion,
           "version matching is case-sensitive and exact");

    candidate.schema_version = {};
    const auto missing_result =
        validate_configuration_schema_version(candidate, supported);
    expect(missing_result.status == ConfigurationValidationStatus::Invalid,
           "missing schema version is invalid");
    expect(has_issue(missing_result, "SCHEMA_VERSION_MISSING"),
           "missing version has a stable reason code");

    candidate.schema_version = "test v1";
    const auto whitespace_result =
        validate_configuration_schema_version(candidate, supported);
    expect(whitespace_result.status == ConfigurationValidationStatus::Invalid,
           "schema version containing whitespace is malformed");
    expect(has_issue(whitespace_result, "SCHEMA_VERSION_MALFORMED"),
           "malformed version has a stable reason code");

    candidate.schema_version = std::string_view{"test\0v1", 7};
    const auto nul_result =
        validate_configuration_schema_version(candidate, supported);
    expect(nul_result.status == ConfigurationValidationStatus::Invalid,
           "schema version containing embedded NUL is malformed");

    candidate.schema_version = "test-v1";
    const ConfigurationSchemaVersionSetView empty_policy{};
    const auto empty_policy_result =
        validate_configuration_schema_version(candidate, empty_policy);
    expect(empty_policy_result.status == ConfigurationValidationStatus::InternalError,
           "empty supported-version policy is an internal configuration error");
    expect(has_issue(empty_policy_result, "SCHEMA_VERSION_POLICY_INVALID"),
           "invalid policy has a stable reason code");

    const ConfigurationSchemaVersionSetView null_nonempty_policy{nullptr, 1};
    expect(validate_configuration_schema_version(candidate, null_nonempty_policy).status ==
               ConfigurationValidationStatus::InternalError,
           "nonzero policy size requires a non-null array");

    const ConfigurationSchemaVersionSetView null_empty_policy{nullptr, 0};
    expect(validate_configuration_schema_version(candidate, null_empty_policy).status ==
               ConfigurationValidationStatus::InternalError,
           "zero-sized policy is not silently treated as support for no versions");

    constexpr std::string_view empty_entry_versions[] = {""};
    const ConfigurationSchemaVersionSetView empty_entry_policy{empty_entry_versions, 1};
    expect(validate_configuration_schema_version(candidate, empty_entry_policy).status ==
               ConfigurationValidationStatus::InternalError,
           "empty entry makes the supported-version policy invalid");

    constexpr std::string_view malformed_entry_versions[] = {"test v1"};
    const ConfigurationSchemaVersionSetView malformed_entry_policy{
        malformed_entry_versions, 1};
    expect(validate_configuration_schema_version(candidate, malformed_entry_policy).status ==
               ConfigurationValidationStatus::InternalError,
           "malformed entry makes the supported-version policy invalid");

    constexpr std::string_view duplicate_versions[] = {"test-v1", "test-v1"};
    const ConfigurationSchemaVersionSetView duplicate_policy{duplicate_versions, 2};
    expect(validate_configuration_schema_version(candidate, duplicate_policy).status ==
               ConfigurationValidationStatus::InternalError,
           "duplicate entries make the supported-version policy invalid");

    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " - " << checks << " checks; failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
