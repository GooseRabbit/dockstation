#include <cstdlib>
#include <iostream>
#include <string_view>

#include "configuration/configuration_manager.hpp"
#include "configuration/configuration_ports.hpp"

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

const ConfigurationIssue kValidationError{
    "TEST_CONFIGURATION_INVALID", std::string_view{"station.mode"},
    ConfigurationIssueSeverity::Error, "Synthetic validation failure."};
const ConfigurationIssue kValidationWarning{
    "TEST_CONFIGURATION_WARNING", std::string_view{"station.optional"},
    ConfigurationIssueSeverity::Warning, "Synthetic optional warning."};
const ConfigurationIssue kReadError{
    "TEST_READ_ERROR", std::nullopt, ConfigurationIssueSeverity::Error,
    "Synthetic repository read error."};
const ConfigurationIssue kSnapshotError{
    "TEST_SNAPSHOT_ERROR", std::nullopt, ConfigurationIssueSeverity::Error,
    "Synthetic snapshot error."};

ConfigurationValidationResult next_validation{};
ConfigurationValidationStatus next_snapshot_status{
    ConfigurationValidationStatus::Valid};
ConfigurationIssueListView next_snapshot_issues{};
bool next_snapshot_present{true};
bool next_snapshot_wrong_identity{false};

int validation_calls = 0;
int snapshot_calls = 0;

ConfigurationCandidate make_candidate() {
    ConfigurationCandidate candidate{};
    candidate.schema_version = "schema-v1";
    candidate.configuration_revision = "revision-17";
    candidate.document.root.type = ConfigurationValueType::Boolean;
    candidate.document.root.value.boolean_value = true;
    return candidate;
}

ConfigurationValidationResult make_validation(
    const ConfigurationCandidate& candidate,
    ConfigurationValidationStatus status,
    ConfigurationIssueListView issues = {}) {
    ConfigurationValidationResult result{};
    result.status = status;
    result.issues = issues;
    result.validated_schema_version = candidate.schema_version;
    result.validated_configuration_revision = candidate.configuration_revision;
    return result;
}

void configure_success(const ConfigurationCandidate& candidate) {
    next_validation = make_validation(
        candidate, ConfigurationValidationStatus::Valid);
    next_snapshot_status = ConfigurationValidationStatus::Valid;
    next_snapshot_issues = {};
    next_snapshot_present = true;
    next_snapshot_wrong_identity = false;
}

class FakeSettingsRepository final : public SettingsRepository {
public:
    SettingsReadResult result{};
    int calls{0};

    SettingsReadResult read_current() noexcept override {
        ++calls;
        return result;
    }
};

class FakeConfigurationDecoder final : public ConfigurationDecoderPort {
public:
    ConfigurationDecodeResult result{};
    int calls{0};
    const std::uint8_t* received_data{nullptr};
    std::size_t received_size{0};

    ConfigurationDecodeResult decode(
        const ConfigurationRecordView& record) noexcept override {
        ++calls;
        received_data = record.data;
        received_size = record.size;
        return result;
    }
};

void reset_pipeline() {
    next_validation = {};
    next_snapshot_status = ConfigurationValidationStatus::Valid;
    next_snapshot_issues = {};
    next_snapshot_present = true;
    next_snapshot_wrong_identity = false;
    validation_calls = 0;
    snapshot_calls = 0;
}

}  // namespace

namespace dockstation::configuration {

// F05/F06 are independently tested. These controlled doubles isolate F07's
// repository/decode/validation/snapshot orchestration contract.
ConfigurationValidationResult validate_configuration(
    const ConfigurationCandidate&,
    const ConfigurationValidationPolicyView&,
    ConfigurationIssue*,
    std::size_t) noexcept {
    ++validation_calls;
    return next_validation;
}

ConfigurationSnapshotResult build_validated_configuration_snapshot(
    const ConfigurationCandidate& candidate,
    const ConfigurationValidationResult&) noexcept {
    ++snapshot_calls;
    std::optional<ValidatedConfigurationSnapshot> snapshot{};
    if (next_snapshot_present) {
        const std::string_view version = next_snapshot_wrong_identity
            ? std::string_view{"other-schema"} : candidate.schema_version;
        const ValidatedConfigurationSnapshot created{
            version, candidate.configuration_revision, candidate.document};
        snapshot = created;
    }
    return {next_snapshot_status, snapshot, next_snapshot_issues};
}

}  // namespace dockstation::configuration

int main() {
    const ConfigurationValidationPolicyView policy{};
    ConfigurationIssue issue_buffer[16]{};
    const std::uint8_t record_bytes[] = {0x10U, 0x20U, 0x30U};
    auto candidate = make_candidate();

    // Happy path: one read, one decode, one combined validation, one snapshot.
    reset_pipeline();
    configure_success(candidate);
    FakeSettingsRepository repository{};
    repository.result = {SettingsReadStatus::Found,
                         {record_bytes, sizeof(record_bytes)}};
    FakeConfigurationDecoder decoder{};
    decoder.result = {ConfigurationDecodeStatus::Decoded, candidate};
    auto result = load_configuration(repository, decoder, policy,
                                     issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::Valid &&
               result.snapshot.has_value(),
           "valid record produces a validated snapshot");
    expect(result.schema_version == candidate.schema_version &&
               result.configuration_revision == candidate.configuration_revision,
           "load result exposes the snapshot's exact schema and revision");
    expect(repository.calls == 1 && decoder.calls == 1 &&
               validation_calls == 1 && snapshot_calls == 1,
           "happy path executes each pipeline stage exactly once");
    expect(decoder.received_data == record_bytes &&
               decoder.received_size == sizeof(record_bytes),
           "decoder receives the exact record view from repository");

    // Warnings survive the whole load path.
    reset_pipeline();
    configure_success(candidate);
    next_validation.issues = {&kValidationWarning, 1};
    next_snapshot_issues = {&kValidationWarning, 1};
    repository = {};
    repository.result = {SettingsReadStatus::Found,
                         {record_bytes, sizeof(record_bytes)}};
    decoder = {};
    decoder.result = {ConfigurationDecodeStatus::Decoded, candidate};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::Valid &&
               result.snapshot.has_value() &&
               result.issues.size == 1 &&
               result.issues.items[0].reason_code == "TEST_CONFIGURATION_WARNING",
           "non-blocking WARNING is preserved with the loaded snapshot");

    // Repository statuses remain distinct and prevent downstream calls.
    reset_pipeline();
    repository = {};
    decoder = {};
    repository.result = {SettingsReadStatus::NotFound, {}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::NotFound &&
               decoder.calls == 0 && validation_calls == 0,
           "NOT_FOUND stops before decoding and validation");

    repository = {};
    repository.result = {SettingsReadStatus::Unavailable, {}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::Unavailable,
           "UNAVAILABLE remains distinct");

    repository = {};
    repository.result = {SettingsReadStatus::ReadError, {}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::ReadError &&
               result.issues.size == 1 &&
               result.issues.items[0].reason_code == "CONFIGURATION_RECORD_READ_ERROR",
           "READ_ERROR has a dedicated diagnostic");

    repository = {};
    repository.result = {SettingsReadStatus::ReadError,
                         {record_bytes, sizeof(record_bytes)}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError &&
               decoder.calls == 0,
           "contradictory repository status and record data fail closed");

    repository = {};
    repository.result = {SettingsReadStatus::Found,
                         {nullptr, sizeof(record_bytes)}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError &&
               decoder.calls == 0,
           "FOUND with null non-empty record fails closed");

    repository = {};
    repository.result = {static_cast<SettingsReadStatus>(255), {}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "unknown repository status fails closed");

    // Decoder statuses map separately.
    reset_pipeline();
    repository = {};
    repository.result = {SettingsReadStatus::Found,
                         {record_bytes, sizeof(record_bytes)}};
    decoder = {};
    decoder.result = {ConfigurationDecodeStatus::InvalidRecord, {}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::DecodeError &&
               validation_calls == 0,
           "INVALID_RECORD maps to DECODE_ERROR");

    decoder = {};
    decoder.result = {ConfigurationDecodeStatus::UnsupportedVersion, {}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::UnsupportedVersion,
           "decoder UNSUPPORTED_VERSION remains distinct");

    decoder = {};
    decoder.result = {ConfigurationDecodeStatus::Error, {}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "decoder ERROR maps to INTERNAL_ERROR");

    decoder = {};
    decoder.result = {static_cast<ConfigurationDecodeStatus>(255), {}};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "unknown decoder status fails closed");

    // F05 outcomes map to ConfigurationLoadStatus without asking F06 to create
    // a snapshot for any non-VALID validation result.
    reset_pipeline();
    configure_success(candidate);
    next_validation = make_validation(
        candidate, ConfigurationValidationStatus::Invalid,
        {&kValidationError, 1});
    decoder = {};
    decoder.result = {ConfigurationDecodeStatus::Decoded, candidate};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::Invalid &&
               !result.snapshot.has_value() && snapshot_calls == 0,
           "INVALID validation does not call snapshot creation");

    reset_pipeline();
    next_validation = make_validation(
        candidate, ConfigurationValidationStatus::UnsupportedVersion,
        {&kValidationError, 1});
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::UnsupportedVersion &&
               !result.snapshot.has_value(),
           "validation UNSUPPORTED_VERSION maps to load status");

    reset_pipeline();
    next_validation = make_validation(
        candidate, ConfigurationValidationStatus::InternalError,
        {&kValidationError, 1});
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError &&
               !result.snapshot.has_value(),
           "validation INTERNAL_ERROR maps to load status");

    reset_pipeline();
    next_validation = make_validation(
        candidate, static_cast<ConfigurationValidationStatus>(255));
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "unknown validation status fails closed");

    reset_pipeline();
    next_validation = make_validation(
        candidate, ConfigurationValidationStatus::Valid);
    next_validation.validated_configuration_revision = "different-revision";
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "validation result bound to another candidate is rejected");

    reset_pipeline();
    next_validation = make_validation(
        candidate, ConfigurationValidationStatus::Valid,
        {&kValidationError, 1});
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "VALID validation result containing ERROR is rejected");

    reset_pipeline();
    next_validation = make_validation(
        candidate, ConfigurationValidationStatus::Invalid);
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "INVALID validation without an ERROR issue is rejected");

    reset_pipeline();
    next_validation = make_validation(
        candidate, ConfigurationValidationStatus::Valid);
    next_validation.issues = {nullptr, 1};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "malformed validation issue list is rejected");

    // F06 outcomes are checked before a load result can be marked VALID.
    reset_pipeline();
    configure_success(candidate);
    next_snapshot_status = ConfigurationValidationStatus::Invalid;
    next_snapshot_issues = {&kValidationError, 1};
    next_snapshot_present = false;
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError &&
               !result.snapshot.has_value(),
           "unexpected snapshot-stage INVALID fails closed as INTERNAL_ERROR");

    reset_pipeline();
    configure_success(candidate);
    next_snapshot_status = static_cast<ConfigurationValidationStatus>(255);
    next_snapshot_issues = {&kSnapshotError, 1};
    next_snapshot_present = false;
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "unknown snapshot status fails closed");

    reset_pipeline();
    configure_success(candidate);
    next_snapshot_present = false;
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "VALID snapshot result without snapshot is rejected");

    reset_pipeline();
    configure_success(candidate);
    next_snapshot_status = ConfigurationValidationStatus::InternalError;
    next_snapshot_issues = {&kSnapshotError, 1};
    next_snapshot_present = true;
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError &&
               !result.snapshot.has_value(),
           "failure snapshot result carrying a snapshot is rejected");

    reset_pipeline();
    configure_success(candidate);
    next_snapshot_wrong_identity = true;
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError &&
               !result.snapshot.has_value(),
           "snapshot carrying another schema identity is rejected");

    reset_pipeline();
    candidate.configuration_revision = "";
    decoder.result = {ConfigurationDecodeStatus::Decoded, candidate};
    configure_success(candidate);
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "snapshot with empty configuration revision is rejected");
    candidate = make_candidate();
    decoder.result = {ConfigurationDecodeStatus::Decoded, candidate};

    reset_pipeline();
    configure_success(candidate);
    next_snapshot_status = ConfigurationValidationStatus::Valid;
    next_snapshot_issues = {&kSnapshotError, 1};
    result = load_configuration(repository, decoder, policy, issue_buffer, 16);
    expect(result.status == ConfigurationLoadStatus::InternalError,
           "VALID snapshot result containing ERROR is rejected");

    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " - " << checks << " checks; failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
