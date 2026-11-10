#include "configuration_manager.hpp"

#include <limits>

namespace dockstation::configuration {
namespace {

const ConfigurationIssue kInvalidPolicyIssue{
    "CONFIGURATION_VALIDATION_POLICY_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "The combined configuration validation policy is incomplete or malformed."};
const ConfigurationIssue kIssueBufferTooSmall{
    "CONFIGURATION_VALIDATION_ISSUE_BUFFER_TOO_SMALL", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "Issue buffer cannot hold the worst-case combined validation issue list."};
const ConfigurationIssue kInvalidSubvalidationResult{
    "CONFIGURATION_VALIDATION_SUBRESULT_INVALID", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "A required configuration validator returned an invalid result contract."};
const ConfigurationIssue kUnexpectedSubvalidationStatus{
    "CONFIGURATION_VALIDATION_SUBSTATUS_UNKNOWN", std::nullopt,
    ConfigurationIssueSeverity::Error,
    "A required configuration validator returned an unsupported status."};

ConfigurationValidationResult make_result(ConfigurationValidationStatus status,
                                           const ConfigurationIssue* issue) noexcept {
    return {status, issue == nullptr ? ConfigurationIssueListView{}
                                    : ConfigurationIssueListView{issue, 1}};
}

bool add_to_issue_capacity(std::size_t value, std::size_t& total) noexcept {
    if (value > std::numeric_limits<std::size_t>::max() - total) return false;
    total += value;
    return true;
}

bool valid_policy_shape(const ConfigurationValidationPolicyView& policy) noexcept {
    return policy.supported_versions.versions != nullptr &&
           policy.supported_versions.size > 0 &&
           policy.required_fields.fields != nullptr &&
           policy.required_fields.size > 0 &&
           policy.value_rules.rules != nullptr &&
           policy.value_rules.size > 0 &&
           policy.consistency_rules.rules != nullptr &&
           policy.consistency_rules.size > 0;
}

// Копирует issue из результата отдельной проверки в общий caller-owned буфер.
// Каждый подвалидатор либо возвращает полный список, либо один статический issue.
void append_subresult(const ConfigurationValidationResult& subresult,
                      ConfigurationIssue* output,
                      std::size_t output_capacity,
                      std::size_t& output_size,
                      bool& has_invalid,
                      bool& has_internal_error) noexcept {
    if (subresult.status == ConfigurationValidationStatus::Invalid) {
        has_invalid = true;
    } else if (subresult.status == ConfigurationValidationStatus::InternalError) {
        has_internal_error = true;
    } else if (subresult.status != ConfigurationValidationStatus::Valid) {
        has_internal_error = true;
        output[output_size++] = kUnexpectedSubvalidationStatus;
        return;
    }

    if (subresult.issues.size == 0) {
        if (subresult.status != ConfigurationValidationStatus::Valid) {
            output[output_size++] = kInvalidSubvalidationResult;
            has_internal_error = true;
        }
        return;
    }
    if (subresult.issues.items == nullptr ||
        subresult.issues.size > output_capacity - output_size) {
        output[output_size++] = kInvalidSubvalidationResult;
        has_internal_error = true;
        return;
    }

    const std::size_t first_output_index = output_size;
    bool has_error_issue = false;
    for (std::size_t i = 0; i < subresult.issues.size; ++i) {
        const ConfigurationIssue& issue = subresult.issues.items[i];
        if (issue.severity == ConfigurationIssueSeverity::Error) {
            has_error_issue = true;
            output[output_size++] = issue;
        } else if (issue.severity == ConfigurationIssueSeverity::Warning) {
            output[output_size++] = issue;
        } else {
            output[output_size++] = kInvalidSubvalidationResult;
            has_internal_error = true;
        }
    }

    if (subresult.status == ConfigurationValidationStatus::Valid && has_error_issue) {
        has_internal_error = true;
    } else if (subresult.status != ConfigurationValidationStatus::Valid &&
               !has_error_issue) {
        // INVALID/INTERNAL_ERROR without at least one ERROR contradicts the
        // validation contract. Replace the first item with an explicit error.
        output[first_output_index] = kInvalidSubvalidationResult;
        has_internal_error = true;
    }
}

}  // namespace

ConfigurationValidationResult validate_configuration(
    const ConfigurationCandidate& candidate,
    const ConfigurationValidationPolicyView& policy,
    ConfigurationIssue* issue_buffer,
    std::size_t issue_capacity) noexcept {
    const auto bind_to_candidate = [&candidate](
        ConfigurationValidationResult result) noexcept {
        result.validated_schema_version = candidate.schema_version;
        result.validated_configuration_revision = candidate.configuration_revision;
        return result;
    };

    // Сначала проверяем версию. Данные неизвестной схемы нельзя проверять
    // по правилам другой/неподдерживаемой схемы.
    const ConfigurationValidationResult schema_result =
        validate_configuration_schema_version(candidate, policy.supported_versions);
    switch (schema_result.status) {
        case ConfigurationValidationStatus::Invalid:
        case ConfigurationValidationStatus::UnsupportedVersion:
        case ConfigurationValidationStatus::InternalError:
            return bind_to_candidate(schema_result);
        case ConfigurationValidationStatus::Valid:
            break;
        default:
            return bind_to_candidate(make_result(
                ConfigurationValidationStatus::InternalError,
                &kUnexpectedSubvalidationStatus));
    }
    if (!valid_policy_shape(policy)) {
        return bind_to_candidate(make_result(
            ConfigurationValidationStatus::InternalError, &kInvalidPolicyIssue));
    }

    // Резервируем worst-case ёмкость до запуска любой проверки, чтобы ошибка
    // размера не оставила частично заполненный буфер.
    // Дополнительный слот позволяет F01 вернуть один WARNING при успешной проверке.
    std::size_t maximum_issue_count = 1;
    if (!add_to_issue_capacity(policy.required_fields.size, maximum_issue_count) ||
        !add_to_issue_capacity(policy.value_rules.size, maximum_issue_count) ||
        !add_to_issue_capacity(policy.consistency_rules.size, maximum_issue_count)) {
        return bind_to_candidate(make_result(
            ConfigurationValidationStatus::InternalError, &kInvalidPolicyIssue));
    }
    if (issue_buffer == nullptr || issue_capacity < maximum_issue_count) {
        return bind_to_candidate(make_result(
            ConfigurationValidationStatus::InternalError, &kIssueBufferTooSmall));
    }

    std::size_t output_size = 0;
    bool has_invalid = false;
    bool has_internal_error = false;

    append_subresult(schema_result, issue_buffer, issue_capacity, output_size,
                     has_invalid, has_internal_error);
    if (has_internal_error) {
        return bind_to_candidate({ConfigurationValidationStatus::InternalError,
                                   ConfigurationIssueListView{issue_buffer, output_size}});
    }

    const ConfigurationValidationResult required_result =
        validate_required_configuration_fields(
            candidate, policy.required_fields,
            issue_buffer + output_size, issue_capacity - output_size);
    append_subresult(required_result, issue_buffer, issue_capacity, output_size,
                     has_invalid, has_internal_error);

    const ConfigurationValidationResult values_result =
        validate_configuration_values(
            candidate, policy.value_rules,
            issue_buffer + output_size, issue_capacity - output_size);
    append_subresult(values_result, issue_buffer, issue_capacity, output_size,
                     has_invalid, has_internal_error);

    const ConfigurationValidationResult consistency_result =
        validate_configuration_consistency(
            candidate, policy.consistency_rules,
            issue_buffer + output_size, issue_capacity - output_size);
    append_subresult(consistency_result, issue_buffer, issue_capacity, output_size,
                     has_invalid, has_internal_error);

    if (has_internal_error) {
        return bind_to_candidate({ConfigurationValidationStatus::InternalError,
                                  ConfigurationIssueListView{issue_buffer, output_size}});
    }
    if (has_invalid) {
        return bind_to_candidate({ConfigurationValidationStatus::Invalid,
                                  ConfigurationIssueListView{issue_buffer, output_size}});
    }
    if (output_size == 0) {
        return bind_to_candidate({ConfigurationValidationStatus::Valid,
                                  ConfigurationIssueListView{}});
    }
    return bind_to_candidate({ConfigurationValidationStatus::Valid,
                              ConfigurationIssueListView{issue_buffer, output_size}});
}

}  // namespace dockstation::configuration
