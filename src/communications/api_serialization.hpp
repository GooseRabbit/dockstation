#pragma once

#include <string>

namespace dockstation::communications {

[[nodiscard]] std::string json_escape(const std::string& value);
[[nodiscard]] std::string serialize_health_response();
[[nodiscard]] std::string serialize_system_diagnostics();
[[nodiscard]] std::string serialize_error_response(
    const std::string& code,
    const std::string& message);

}  // namespace dockstation::communications
