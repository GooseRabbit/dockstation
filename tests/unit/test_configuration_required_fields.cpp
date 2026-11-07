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
    if (condition) std::cout << "PASS - " << name << '\n';
    else { std::cerr << "FAIL - " << name << '\n'; ++failures; }
}
bool has_issue(const ConfigurationValidationResult& result,
               std::string_view reason_code, std::string_view field_path) {
    if (result.issues.items == nullptr) return false;
    for (std::size_t i = 0; i < result.issues.size; ++i) {
        const ConfigurationIssue& issue = result.issues.items[i];
        if (issue.reason_code == reason_code && issue.field_path.has_value() &&
            *issue.field_path == field_path) return true;
    }
    return false;
}
ConfigurationValueView make_object(const ConfigurationObjectEntryView* entries,
                                   std::size_t size) {
    ConfigurationValueView value{};
    value.type = ConfigurationValueType::Object;
    value.value.object_value = {entries, size};
    return value;
}
ConfigurationValueView make_string(std::string_view text) {
    ConfigurationValueView value{};
    value.type = ConfigurationValueType::String;
    value.value.string_value = text;
    return value;
}
}  // namespace

int main() {
    constexpr std::string_view device_id_segments[] = {"device", "id"};
    constexpr std::string_view sensor_port_segments[] = {"sensors", "port"};
    constexpr std::string_view optional_segments[] = {"optional", "feature"};
    constexpr std::string_view device_label_segments[] = {"device", "label"};
    const ConfigurationRequiredFieldPathView required_paths[] = {
        {device_id_segments, 2, "device.id"}, {sensor_port_segments, 2, "sensors.port"}};
    const ConfigurationRequiredFieldSetView required_set{required_paths, 2};

    ConfigurationValueView null_value{};
    null_value.type = ConfigurationValueType::Null;
    ConfigurationValueView unknown_value{};
    unknown_value.type = ConfigurationValueType::Unknown;
    const ConfigurationObjectEntryView device_entries[] = {
        {"id", null_value}, {"label", unknown_value}};
    const ConfigurationObjectEntryView sensor_entries[] = {{"port", make_string("UART-1")}};
    const ConfigurationObjectEntryView root_entries[] = {
        {"device", make_object(device_entries, 2)},
        {"sensors", make_object(sensor_entries, 1)}};

    ConfigurationCandidate candidate{};
    candidate.document.root = make_object(root_entries, 2);
    ConfigurationIssue issue_buffer[4]{};

    const auto valid_result =
        validate_required_configuration_fields(candidate, required_set, issue_buffer, 4);
    expect(valid_result.status == ConfigurationValidationStatus::Valid,
           "all explicitly required nested fields are present");
    expect(valid_result.issues.size == 0 && valid_result.issues.items == nullptr,
           "valid required-field result has no issues");
    expect(validate_required_configuration_fields(candidate, required_set, nullptr, 0).status ==
               ConfigurationValidationStatus::Valid,
           "fields outside the required set do not affect validation");

    const ConfigurationRequiredFieldPathView nullable_required[] = {
        {device_id_segments, 2, "device.id"}};
    const ConfigurationRequiredFieldSetView nullable_set{nullable_required, 1};
    expect(validate_required_configuration_fields(candidate, nullable_set, nullptr, 0).status ==
               ConfigurationValidationStatus::Valid,
           "a present Null value satisfies presence-only validation");

    const ConfigurationRequiredFieldPathView unknown_required[] = {
        {device_label_segments, 2, "device.label"}};
    const ConfigurationRequiredFieldSetView unknown_set{unknown_required, 1};
    expect(validate_required_configuration_fields(candidate, unknown_set, nullptr, 0).status ==
               ConfigurationValidationStatus::Valid,
           "a present Unknown value satisfies presence-only validation");

    candidate.document.root = make_object(nullptr, 0);
    const auto missing_result =
        validate_required_configuration_fields(candidate, required_set, issue_buffer, 4);
    expect(missing_result.status == ConfigurationValidationStatus::Invalid,
           "missing required fields make the candidate invalid");
    expect(missing_result.issues.size == 2, "every missing required field is reported");
    expect(has_issue(missing_result, "REQUIRED_FIELD_MISSING", "device.id") &&
               has_issue(missing_result, "REQUIRED_FIELD_MISSING", "sensors.port"),
           "missing-field issues include their diagnostic paths");

    const ConfigurationRequiredFieldPathView optional_required[] = {
        {optional_segments, 2, "optional.feature"}};
    const ConfigurationRequiredFieldSetView optional_set{optional_required, 1};
    ConfigurationIssue optional_issue_buffer[1]{};
    const auto optional_missing_result =
        validate_required_configuration_fields(candidate, optional_set,
                                               optional_issue_buffer, 1);
    expect(optional_missing_result.status == ConfigurationValidationStatus::Invalid,
           "a field is required only when included in the explicit policy");

    const ConfigurationObjectEntryView non_object_root_entries[] = {
        {"device", make_string("not-an-object")}};
    candidate.document.root = make_object(non_object_root_entries, 1);
    const ConfigurationRequiredFieldPathView device_only[] = {
        {device_id_segments, 2, "device.id"}};
    const ConfigurationRequiredFieldSetView device_only_set{device_only, 1};
    const auto unresolvable_result =
        validate_required_configuration_fields(candidate, device_only_set, issue_buffer, 4);
    expect(unresolvable_result.status == ConfigurationValidationStatus::Invalid,
           "non-object parent makes required path invalid");
    expect(has_issue(unresolvable_result, "REQUIRED_FIELD_PATH_UNRESOLVABLE", "device.id"),
           "non-object parent has a distinct diagnostic reason");

    const ConfigurationObjectEntryView duplicate_device_entries[] = {
        {"id", make_string("A")}, {"id", make_string("B")}};
    const ConfigurationObjectEntryView duplicate_root_entries[] = {
        {"device", make_object(duplicate_device_entries, 2)}};
    candidate.document.root = make_object(duplicate_root_entries, 1);
    const auto ambiguous_result =
        validate_required_configuration_fields(candidate, device_only_set, issue_buffer, 4);
    expect(ambiguous_result.status == ConfigurationValidationStatus::Invalid,
           "duplicate object keys make required path invalid");
    expect(has_issue(ambiguous_result, "REQUIRED_FIELD_PATH_AMBIGUOUS", "device.id"),
           "duplicate object keys have a distinct diagnostic reason");

    ConfigurationValueView broken_object{};
    broken_object.type = ConfigurationValueType::Object;
    broken_object.value.object_value = {nullptr, 1};
    candidate.document.root = broken_object;
    const auto broken_document_result =
        validate_required_configuration_fields(candidate, device_only_set, issue_buffer, 4);
    expect(broken_document_result.status == ConfigurationValidationStatus::InternalError,
           "invalid object pointer is an internal error, not a missing-field result");

    candidate.document.root = make_object(nullptr, 0);
    const auto small_buffer_result =
        validate_required_configuration_fields(candidate, required_set, issue_buffer, 1);
    expect(small_buffer_result.status == ConfigurationValidationStatus::InternalError,
           "undersized issue buffer fails closed");
    expect(small_buffer_result.issues.size == 1 &&
               small_buffer_result.issues.items[0].reason_code ==
                   "REQUIRED_FIELD_ISSUE_BUFFER_TOO_SMALL",
           "undersized issue buffer returns one static issue, not partial field issues");

    const auto null_buffer_result =
        validate_required_configuration_fields(candidate, required_set, nullptr, 2);
    expect(null_buffer_result.status == ConfigurationValidationStatus::InternalError &&
               null_buffer_result.issues.size == 1,
           "null issue buffer fails closed when issues are required");

    const ConfigurationRequiredFieldSetView empty_policy{};
    expect(validate_required_configuration_fields(candidate, empty_policy, issue_buffer, 4).status ==
               ConfigurationValidationStatus::InternalError,
           "empty required-field policy is not mistaken for a completed validation");

    const ConfigurationRequiredFieldSetView null_nonempty_policy{nullptr, 1};
    expect(validate_required_configuration_fields(candidate, null_nonempty_policy,
                                                   issue_buffer, 4).status ==
               ConfigurationValidationStatus::InternalError,
           "nonempty policy requires a non-null field array");

    const ConfigurationRequiredFieldPathView null_segments[] = {{nullptr, 1, "device.id"}};
    const ConfigurationRequiredFieldSetView null_segments_set{null_segments, 1};
    expect(validate_required_configuration_fields(candidate, null_segments_set,
                                                   issue_buffer, 4).status ==
               ConfigurationValidationStatus::InternalError,
           "required path with null segment array is invalid policy");

    const ConfigurationRequiredFieldPathView zero_segments[] = {
        {device_id_segments, 0, "device.id"}};
    const ConfigurationRequiredFieldSetView zero_segments_set{zero_segments, 1};
    expect(validate_required_configuration_fields(candidate, zero_segments_set,
                                                   issue_buffer, 4).status ==
               ConfigurationValidationStatus::InternalError,
           "required path with zero segments is invalid policy");

    const std::string_view empty_segment_names[] = {"device", ""};
    const ConfigurationRequiredFieldPathView empty_segment_path[] = {
        {empty_segment_names, 2, "device."}};
    const ConfigurationRequiredFieldSetView empty_segment_set{empty_segment_path, 1};
    expect(validate_required_configuration_fields(candidate, empty_segment_set,
                                                   issue_buffer, 4).status ==
               ConfigurationValidationStatus::InternalError,
           "empty path segment is invalid policy");

    const ConfigurationRequiredFieldPathView empty_diagnostic_path[] = {
        {device_id_segments, 2, ""}};
    const ConfigurationRequiredFieldSetView empty_diagnostic_set{empty_diagnostic_path, 1};
    expect(validate_required_configuration_fields(candidate, empty_diagnostic_set,
                                                   issue_buffer, 4).status ==
               ConfigurationValidationStatus::InternalError,
           "empty diagnostic path is invalid policy");

    const ConfigurationRequiredFieldPathView duplicate_paths[] = {
        {device_id_segments, 2, "device.id"}, {device_id_segments, 2, "device.id"}};
    const ConfigurationRequiredFieldSetView duplicate_set{duplicate_paths, 2};
    expect(validate_required_configuration_fields(candidate, duplicate_set,
                                                   issue_buffer, 4).status ==
               ConfigurationValidationStatus::InternalError,
           "duplicate required paths are invalid policy");

    candidate.document.root = make_string("root-scalar");
    const auto scalar_root_result =
        validate_required_configuration_fields(candidate, device_only_set, issue_buffer, 4);
    expect(scalar_root_result.status == ConfigurationValidationStatus::Invalid &&
               has_issue(scalar_root_result, "REQUIRED_FIELD_PATH_UNRESOLVABLE", "device.id"),
           "non-object document root cannot satisfy an object field path");

    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " - " << checks << " checks; failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
