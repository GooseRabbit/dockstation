#include <cstdlib>
#include <iostream>
#include <string>

#include "platform/linux/runtime_config.hpp"

using namespace dockstation::platform::linux_runtime;

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
}  // namespace

int main() {
    RuntimeConfig config{};
    expect(config.api_port == 8080U, "default API port is 8080");
    expect(std::string(kGatewayBindAddress) == "127.0.0.1",
           "Gateway bind address is fixed to loopback");
    expect(validate_runtime_config(config).status ==
               RuntimeConfigValidationStatus::Valid,
           "default runtime configuration is valid");

    config.api_port = 0U;
    auto result = validate_runtime_config(config);
    expect(result.status == RuntimeConfigValidationStatus::Invalid &&
               result.field == "api_port",
           "rejects port zero");

    config.api_port = 65536U;
    result = validate_runtime_config(config);
    expect(result.status == RuntimeConfigValidationStatus::Invalid &&
               result.field == "api_port",
           "rejects port above TCP/UDP range");

    config = RuntimeConfig{};
    config.logger.write_to_stderr = false;
    result = validate_runtime_config(config);
    expect(result.status == RuntimeConfigValidationStatus::Invalid &&
               result.field == "logger",
           "rejects configuration without a log sink");

    config = RuntimeConfig{};
    config.logger.file_path = "/var/log/dockstation/gateway.jsonl";
    config.logger.max_file_size_bytes = 0U;
    result = validate_runtime_config(config);
    expect(result.status == RuntimeConfigValidationStatus::Invalid &&
               result.field == "logger.max_file_size_bytes",
           "rejects zero maximum file size when file sink is enabled");

    config = RuntimeConfig{};
    config.logger.max_rotated_files = 21U;
    result = validate_runtime_config(config);
    expect(result.status == RuntimeConfigValidationStatus::Invalid &&
               result.field == "logger.max_rotated_files",
           "rejects excessive archive count");

    config = RuntimeConfig{};
    config.logger.minimum_level = static_cast<dockstation::platform::logging::LogLevel>(255U);
    result = validate_runtime_config(config);
    expect(result.status == RuntimeConfigValidationStatus::Invalid &&
               result.field == "logger.minimum_level",
           "rejects unknown log level");

    config = RuntimeConfig{};
    config.logger.file_path = std::string("/tmp/gateway") + '\0' + ".log";
    result = validate_runtime_config(config);
    expect(result.status == RuntimeConfigValidationStatus::Invalid &&
               result.field == "logger.file_path",
           "rejects embedded null byte in log path");

    config = RuntimeConfig{};
    config.logger.file_path = "/var/log/dockstation/gateway.jsonl";
    config.logger.max_file_size_bytes = 4096U;
    result = validate_runtime_config(config);
    expect(result.status == RuntimeConfigValidationStatus::Valid,
           "accepts valid optional file sink");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
