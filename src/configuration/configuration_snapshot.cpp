#include "configuration_manager.hpp"

namespace dockstation::configuration {
namespace {

const ConfigurationIssue kInvalidValidationResult{
    "CONFIGURATION_SNAPSHOT_VALIDATION_RESULT_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "The validation result is malformed or contradicts its status."};

const ConfigurationIssue kCandidateBindingMismatch{
    "CONFIGURATION_SNAPSHOT_CANDIDATE_BINDING_MISMATCH", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "The validation result does not identify this exact schema version and configuration revision."};

const ConfigurationIssue kUnknownValidationStatus{
    "CONFIGURATION_SNAPSHOT_VALIDATION_STATUS_UNKNOWN", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "The validation result contains an unsupported status."};

ConfigurationSnapshotResult make_error(const ConfigurationIssue& issue) noexcept {
    return {ConfigurationValidationStatus::InternalError,
            std::nullopt,
            ConfigurationIssueListView{&issue, 1}};
}

bool validate_issue_list(const ConfigurationIssueListView& issues,
                         bool& has_error) noexcept {
    has_error = false;
    if (issues.size > 0 && issues.items == nullptr) return false;

    for (std::size_t i = 0; i < issues.size; ++i) {
        const ConfigurationIssue& issue = issues.items[i];
        if (issue.reason_code.empty() || issue.description.empty() ||
            (issue.field_path.has_value() && issue.field_path->empty())) {
            return false;
        }

        switch (issue.severity) {
            case ConfigurationIssueSeverity::Error:
                has_error = true;
                break;
            case ConfigurationIssueSeverity::Warning:
                break;
            default:
                return false;
        }
    }
    return true;
}

}  // namespace

ConfigurationSnapshotResult build_validated_configuration_snapshot(
    const ConfigurationCandidate& candidate,
    const ConfigurationValidationResult& validation) noexcept {
    bool has_error_issue = false;
    if (!validate_issue_list(validation.issues, has_error_issue)) {
        return make_error(kInvalidValidationResult);
    }

    switch (validation.status) {
        case ConfigurationValidationStatus::Invalid:
        case ConfigurationValidationStatus::UnsupportedVersion:
        case ConfigurationValidationStatus::InternalError:
            if (!has_error_issue) return make_error(kInvalidValidationResult);
            return {validation.status, std::nullopt, validation.issues};

        case ConfigurationValidationStatus::Valid:
            if (has_error_issue) return make_error(kInvalidValidationResult);
            break;

        default:
            return make_error(kUnknownValidationStatus);
    }

    // A VALID result must be tied to the exact candidate that CFG-F05 checked.
    // A non-empty revision is required to identify the configuration snapshot.
    if (candidate.schema_version.empty() ||
        candidate.configuration_revision.empty() ||
        validation.validated_schema_version != candidate.schema_version ||
        validation.validated_configuration_revision != candidate.configuration_revision) {
        return make_error(kCandidateBindingMismatch);
    }

    // Construct locally while inside the friend function; the snapshot's
    // constructor remains inaccessible to ordinary consumers.
    const ValidatedConfigurationSnapshot snapshot{
        candidate.schema_version,
        candidate.configuration_revision,
        candidate.document};

    return {ConfigurationValidationStatus::Valid,
            std::optional<ValidatedConfigurationSnapshot>{snapshot},
            validation.issues};
}

}  // namespace dockstation::configuration
