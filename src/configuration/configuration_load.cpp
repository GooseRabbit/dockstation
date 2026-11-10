#include "configuration_manager.hpp"

#include "configuration_ports.hpp"

namespace dockstation::configuration {
namespace {

const ConfigurationIssue kRepositoryResultInvalid{
    "CONFIGURATION_REPOSITORY_RESULT_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "SettingsRepository returned a contradictory or malformed read result."};

const ConfigurationIssue kUnknownReadStatus{
    "CONFIGURATION_REPOSITORY_STATUS_UNKNOWN", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "SettingsRepository returned an unsupported read status."};

const ConfigurationIssue kRecordViewInvalid{
    "CONFIGURATION_RECORD_VIEW_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "SettingsRepository reported a record with a nonzero size and null data."};

const ConfigurationIssue kReadErrorIssue{
    "CONFIGURATION_RECORD_READ_ERROR", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "SettingsRepository could not read the saved configuration record."};

const ConfigurationIssue kDecodeErrorIssue{
    "CONFIGURATION_RECORD_DECODE_ERROR", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "ConfigurationDecoderPort could not decode the saved record."};

const ConfigurationIssue kInvalidRecordIssue{
    "CONFIGURATION_RECORD_INVALID",
    std::nullopt,
    ConfigurationIssueSeverity::Error,
    "The saved record is not a valid record for the decoder."};

const ConfigurationIssue kDecoderUnsupportedVersionIssue{
    "CONFIGURATION_DECODER_VERSION_UNSUPPORTED", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "ConfigurationDecoderPort identified an unsupported schema version."};

const ConfigurationIssue kDecoderStatusUnknown{
    "CONFIGURATION_DECODER_STATUS_UNKNOWN", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "ConfigurationDecoderPort returned an unsupported decode status."};

const ConfigurationIssue kValidationResultInvalid{
    "CONFIGURATION_LOAD_VALIDATION_RESULT_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Combined validation returned a malformed or contradictory result."};

const ConfigurationIssue kSnapshotResultInvalid{
    "CONFIGURATION_LOAD_SNAPSHOT_RESULT_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Snapshot creation returned a malformed or contradictory result."};

const ConfigurationIssue kSnapshotMissing{
    "CONFIGURATION_LOAD_SNAPSHOT_MISSING", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Validation succeeded but no validated configuration snapshot was returned."};

const ConfigurationIssue kSnapshotIdentityMismatch{
    "CONFIGURATION_LOAD_SNAPSHOT_IDENTITY_MISMATCH", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "The returned snapshot does not identify the decoded candidate."};

const ConfigurationIssue kUnknownValidationStatus{
    "CONFIGURATION_LOAD_VALIDATION_STATUS_UNKNOWN", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Combined validation returned an unsupported status."};

ConfigurationLoadResult make_result(
    ConfigurationLoadStatus status,
    const ConfigurationIssueListView& issues = {}) noexcept {
    ConfigurationLoadResult result{};
    result.status = status;
    result.issues = issues;
    return result;
}

ConfigurationLoadResult make_error(const ConfigurationIssue& issue) noexcept {
    return make_result(ConfigurationLoadStatus::InternalError,
                       ConfigurationIssueListView{&issue, 1});
}

void attach_candidate_identity(ConfigurationLoadResult& result,
                               const ConfigurationCandidate& candidate) noexcept {
    if (!candidate.schema_version.empty()) {
        result.schema_version = candidate.schema_version;
    }
    if (!candidate.configuration_revision.empty()) {
        result.configuration_revision = candidate.configuration_revision;
    }
}

bool has_record_payload(const ConfigurationRecordView& record) noexcept {
    return record.data != nullptr || record.size != 0;
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

bool validation_result_matches_candidate(
    const ConfigurationCandidate& candidate,
    const ConfigurationValidationResult& validation) noexcept {
    if (validation.validated_schema_version != candidate.schema_version ||
        validation.validated_configuration_revision != candidate.configuration_revision) {
        return false;
    }

    bool has_error = false;
    if (!validate_issue_list(validation.issues, has_error)) return false;

    switch (validation.status) {
        case ConfigurationValidationStatus::Valid:
            return !has_error;
        case ConfigurationValidationStatus::Invalid:
        case ConfigurationValidationStatus::UnsupportedVersion:
        case ConfigurationValidationStatus::InternalError:
            return has_error;
    }
    return false;
}

ConfigurationLoadStatus load_status_from_validation(
    ConfigurationValidationStatus status) noexcept {
    switch (status) {
        case ConfigurationValidationStatus::Valid:
            return ConfigurationLoadStatus::Valid;
        case ConfigurationValidationStatus::Invalid:
            return ConfigurationLoadStatus::Invalid;
        case ConfigurationValidationStatus::UnsupportedVersion:
            return ConfigurationLoadStatus::UnsupportedVersion;
        case ConfigurationValidationStatus::InternalError:
            return ConfigurationLoadStatus::InternalError;
    }
    return ConfigurationLoadStatus::InternalError;
}

}  // namespace

ConfigurationLoadResult load_configuration(
    SettingsRepository& repository,
    ConfigurationDecoderPort& decoder,
    const ConfigurationValidationPolicyView& policy,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept {
    const SettingsReadResult read_result = repository.read_current();

    switch (read_result.status) {
        case SettingsReadStatus::NotFound:
            if (has_record_payload(read_result.record)) {
                return make_error(kRepositoryResultInvalid);
            }
            return make_result(ConfigurationLoadStatus::NotFound);

        case SettingsReadStatus::Unavailable:
            if (read_result.record.size != 0) {
                return make_error(kRepositoryResultInvalid);
            }
            return make_result(ConfigurationLoadStatus::Unavailable);

        case SettingsReadStatus::ReadError:
            if (read_result.record.size != 0) {
                return make_error(kRepositoryResultInvalid);
            }
            return make_result(
                ConfigurationLoadStatus::ReadError,
                ConfigurationIssueListView{&kReadErrorIssue, 1});

        case SettingsReadStatus::Found:
            break;

        default:
            return make_error(kUnknownReadStatus);
    }

    if (read_result.record.size > 0 && read_result.record.data == nullptr) {
        return make_error(kRecordViewInvalid);
    }

    // The repository's record view is valid through this decode call. The
    // decoder must keep candidate backing storage alive beyond this call.
    const ConfigurationDecodeResult decode_result = decoder.decode(read_result.record);

    switch (decode_result.status) {
        case ConfigurationDecodeStatus::InvalidRecord:
            return make_result(
                ConfigurationLoadStatus::DecodeError,
                ConfigurationIssueListView{&kInvalidRecordIssue, 1});

        case ConfigurationDecodeStatus::UnsupportedVersion:
            return make_result(
                ConfigurationLoadStatus::UnsupportedVersion,
                ConfigurationIssueListView{&kDecoderUnsupportedVersionIssue, 1});

        case ConfigurationDecodeStatus::Error:
            return make_result(
                ConfigurationLoadStatus::InternalError,
                ConfigurationIssueListView{&kDecodeErrorIssue, 1});

        case ConfigurationDecodeStatus::Decoded:
            break;

        default:
            return make_error(kDecoderStatusUnknown);
    }

    const ConfigurationCandidate& candidate = decode_result.candidate;
    const ConfigurationValidationResult validation = validate_configuration(
        candidate, policy, issue_buffer, issue_capacity);

    if (!validation_result_matches_candidate(candidate, validation)) {
        return make_error(kValidationResultInvalid);
    }

    if (validation.status != ConfigurationValidationStatus::Valid) {
        ConfigurationLoadResult result = make_result(
            load_status_from_validation(validation.status), validation.issues);
        attach_candidate_identity(result, candidate);
        return result;
    }

    const ConfigurationSnapshotResult snapshot_result =
        build_validated_configuration_snapshot(candidate, validation);

    bool snapshot_has_error = false;
    if (!validate_issue_list(snapshot_result.issues, snapshot_has_error)) {
        return make_error(kSnapshotResultInvalid);
    }

    if (snapshot_result.status != ConfigurationValidationStatus::Valid) {
        // F06 is called only after F05 returned VALID. Therefore only an
        // INTERNAL_ERROR is a legitimate failure status at this stage.
        if (snapshot_result.status != ConfigurationValidationStatus::InternalError ||
            snapshot_result.snapshot.has_value() || !snapshot_has_error) {
            return make_error(kSnapshotResultInvalid);
        }

        ConfigurationLoadResult result = make_result(
            ConfigurationLoadStatus::InternalError, snapshot_result.issues);
        attach_candidate_identity(result, candidate);
        return result;
    }

    if (snapshot_has_error || !snapshot_result.snapshot.has_value()) {
        return make_error(kSnapshotMissing);
    }

    const ValidatedConfigurationSnapshot& snapshot = *snapshot_result.snapshot;
    if (snapshot.schema_version().empty() ||
        snapshot.configuration_revision().empty() ||
        snapshot.schema_version() != candidate.schema_version ||
        snapshot.configuration_revision() != candidate.configuration_revision) {
        return make_error(kSnapshotIdentityMismatch);
    }

    ConfigurationLoadResult result = make_result(
        ConfigurationLoadStatus::Valid, snapshot_result.issues);
    result.schema_version = snapshot.schema_version();
    result.configuration_revision = snapshot.configuration_revision();
    result.snapshot = snapshot;
    return result;
}

}  // namespace dockstation::configuration
