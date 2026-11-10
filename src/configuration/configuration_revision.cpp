#include "configuration_manager.hpp"

namespace dockstation::configuration {

std::optional<std::string_view> get_configuration_revision(
    const ConfigurationLoadResult& active_configuration) noexcept {
    if (active_configuration.status != ConfigurationLoadStatus::Valid ||
        !active_configuration.snapshot.has_value() ||
        !active_configuration.schema_version.has_value() ||
        !active_configuration.configuration_revision.has_value()) {
        return std::nullopt;
    }

    const ValidatedConfigurationSnapshot& snapshot =
        *active_configuration.snapshot;
    const std::string_view schema_version = snapshot.schema_version();
    const std::string_view revision = snapshot.configuration_revision();

    if (schema_version.empty() || revision.empty() ||
        active_configuration.schema_version->empty() ||
        active_configuration.configuration_revision->empty() ||
        *active_configuration.schema_version != schema_version ||
        *active_configuration.configuration_revision != revision) {
        return std::nullopt;
    }

    const ConfigurationIssueListView& issues = active_configuration.issues;
    if (issues.size > 0 && issues.items == nullptr) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < issues.size; ++index) {
        const ConfigurationIssue& issue = issues.items[index];
        if (issue.severity != ConfigurationIssueSeverity::Warning ||
            issue.reason_code.empty() || issue.description.empty() ||
            (issue.field_path.has_value() && issue.field_path->empty())) {
            return std::nullopt;
        }
    }

    return revision;
}

}  // namespace dockstation::configuration
