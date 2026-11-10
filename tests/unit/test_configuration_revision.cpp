#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "configuration_manager.hpp"

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

const ConfigurationIssue kWarning{
    "OPTIONAL_FIELD_OMITTED", std::string_view{"station.optional"},
    ConfigurationIssueSeverity::Warning, "An optional field was omitted."};
const ConfigurationIssue kError{
    "CONFIGURATION_INVALID", std::string_view{"station.mode"},
    ConfigurationIssueSeverity::Error, "A required field is invalid."};

ConfigurationLoadResult make_valid_active(
    ConfigurationIssueListView issues = {}) {
    ConfigurationCandidate candidate{};
    candidate.schema_version = "schema-v1";
    candidate.configuration_revision = "revision-42";
    candidate.document.root.type = ConfigurationValueType::Boolean;
    candidate.document.root.value.boolean_value = true;

    ConfigurationValidationResult validation{};
    validation.status = ConfigurationValidationStatus::Valid;
    validation.issues = issues;
    validation.validated_schema_version = candidate.schema_version;
    validation.validated_configuration_revision = candidate.configuration_revision;

    const ConfigurationSnapshotResult snapshot_result =
        build_validated_configuration_snapshot(candidate, validation);

    ConfigurationLoadResult active{};
    active.status = ConfigurationLoadStatus::Valid;
    active.schema_version = candidate.schema_version;
    active.configuration_revision = candidate.configuration_revision;
    active.issues = snapshot_result.issues;
    active.snapshot = snapshot_result.snapshot;
    return active;
}

}  // namespace

int main() {
    ConfigurationLoadResult active = make_valid_active();
    auto revision = get_configuration_revision(active);
    expect(revision.has_value() && *revision == "revision-42",
           "VALID active snapshot returns its exact revision");

    const ConfigurationIssue warning[] = {kWarning};
    active = make_valid_active({warning, 1});
    revision = get_configuration_revision(active);
    expect(revision.has_value() && *revision == "revision-42",
           "a well-formed non-blocking WARNING does not hide the revision");

    active = make_valid_active();
    active.status = ConfigurationLoadStatus::NotFound;
    expect(!get_configuration_revision(active).has_value(),
           "NOT_FOUND has no active configuration revision");
    active.status = ConfigurationLoadStatus::Unavailable;
    expect(!get_configuration_revision(active).has_value(),
           "UNAVAILABLE has no active configuration revision");
    active.status = ConfigurationLoadStatus::Invalid;
    expect(!get_configuration_revision(active).has_value(),
           "INVALID has no active configuration revision");
    active.status = ConfigurationLoadStatus::InternalError;
    expect(!get_configuration_revision(active).has_value(),
           "INTERNAL_ERROR has no active configuration revision");

    active = make_valid_active();
    active.snapshot.reset();
    expect(!get_configuration_revision(active).has_value(),
           "VALID without a snapshot fails closed");

    active = make_valid_active();
    active.schema_version.reset();
    expect(!get_configuration_revision(active).has_value(),
           "missing load-result schema metadata fails closed");
    active = make_valid_active();
    active.configuration_revision.reset();
    expect(!get_configuration_revision(active).has_value(),
           "missing load-result revision metadata fails closed");
    active = make_valid_active();
    active.schema_version = std::string_view{};
    expect(!get_configuration_revision(active).has_value(),
           "empty load-result schema metadata fails closed");
    active = make_valid_active();
    active.configuration_revision = std::string_view{};
    expect(!get_configuration_revision(active).has_value(),
           "empty load-result revision metadata fails closed");

    active = make_valid_active();
    active.schema_version = "different-schema";
    expect(!get_configuration_revision(active).has_value(),
           "schema metadata mismatch with snapshot fails closed");
    active = make_valid_active();
    active.configuration_revision = "different-revision";
    expect(!get_configuration_revision(active).has_value(),
           "revision metadata mismatch with snapshot fails closed");

    active = make_valid_active();
    active.issues = {nullptr, 1};
    expect(!get_configuration_revision(active).has_value(),
           "malformed issue-list storage fails closed");
    const ConfigurationIssue error[] = {kError};
    active = make_valid_active({error, 1});
    expect(!get_configuration_revision(active).has_value(),
           "ERROR issue prevents reporting an active revision");

    ConfigurationIssue unknown_severity = kWarning;
    unknown_severity.severity = static_cast<ConfigurationIssueSeverity>(255);
    active = make_valid_active({&unknown_severity, 1});
    expect(!get_configuration_revision(active).has_value(),
           "unknown issue severity fails closed");

    ConfigurationIssue empty_reason = kWarning;
    empty_reason.reason_code = "";
    active = make_valid_active({&empty_reason, 1});
    expect(!get_configuration_revision(active).has_value(),
           "warning without a reason code fails closed");
    ConfigurationIssue empty_description = kWarning;
    empty_description.description = "";
    active = make_valid_active({&empty_description, 1});
    expect(!get_configuration_revision(active).has_value(),
           "warning without a description fails closed");
    ConfigurationIssue empty_path = kWarning;
    empty_path.field_path = std::string_view{};
    active = make_valid_active({&empty_path, 1});
    expect(!get_configuration_revision(active).has_value(),
           "warning with an empty field path fails closed");

    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " - " << checks << " checks; failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
