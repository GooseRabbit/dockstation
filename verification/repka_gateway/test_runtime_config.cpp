#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

#include "platform/logging/logger.hpp"

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

    const std::string log_path = "/tmp/dockstation_runtime_logger_" +
                                 std::to_string(static_cast<long long>(::getpid())) + ".jsonl";
    std::remove(log_path.c_str());

    RuntimeConfig invalid_config{};
    invalid_config.api_port = 0U;
    auto initialization = initialize_runtime_logger(invalid_config);
    expect(initialization.status == RuntimeLoggerInitializationStatus::Rejected &&
               initialization.field == "api_port",
           "does not initialize logger when runtime configuration is invalid");

    RuntimeConfig logger_config{};
    logger_config.logger.write_to_stderr = false;
    logger_config.logger.file_path = log_path;
    logger_config.logger.max_file_size_bytes = 4096U;
    initialization = initialize_runtime_logger(logger_config);
    expect(initialization.status == RuntimeLoggerInitializationStatus::Ready,
           "initializes PlatformLogger after successful runtime validation");
    expect(dockstation::platform::logging::write_log_record(
               dockstation::platform::logging::LogLevel::Info,
               "runtime_config",
               "runtime logger integration test").status ==
               dockstation::platform::logging::LoggerStatus::Available,
           "initialized runtime logger accepts a log record");
    expect(dockstation::platform::logging::shutdown_logger().status ==
               dockstation::platform::logging::LoggerStatus::Available,
           "runtime logger integration shuts down cleanly");

    std::ifstream log_input(log_path);
    const std::string log_content(std::istreambuf_iterator<char>(log_input),
                                  std::istreambuf_iterator<char>());
    expect(log_content.find("runtime logger integration test") != std::string::npos,
           "runtime logger integration writes to configured file");
    std::remove(log_path.c_str());

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
