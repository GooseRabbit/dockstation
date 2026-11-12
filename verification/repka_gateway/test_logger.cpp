#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

#include "platform/logging/logger.hpp"

using namespace dockstation::platform::logging;

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

std::string read_file(const std::string& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}
}  // namespace

int main() {
    const std::string base = "/tmp/dockstation_logger_test_" +
                             std::to_string(static_cast<long long>(::getpid())) + ".log";
    std::remove(base.c_str());
    std::remove((base + ".1").c_str());
    std::remove((base + ".2").c_str());

    LoggerConfig invalid{};
    invalid.write_to_stderr = false;
    expect(initialize_logger(invalid).status == LoggerStatus::Unavailable,
           "rejects configuration without a sink");

    LoggerConfig invalid_level{};
    invalid_level.minimum_level = static_cast<LogLevel>(255U);
    expect(initialize_logger(invalid_level).status == LoggerStatus::Unavailable,
           "rejects unknown configured log level");

    LoggerConfig invalid_path{};
    invalid_path.file_path = std::string("/tmp/log") + '\0' + ".jsonl";
    expect(initialize_logger(invalid_path).status == LoggerStatus::Unavailable,
           "rejects embedded null byte in log file path");

    LoggerConfig config{};
    config.minimum_level = LogLevel::Debug;
    config.file_path = base;
    config.max_file_size_bytes = 180U;
    config.max_rotated_files = 2U;
    config.write_to_stderr = false;
    expect(initialize_logger(config).status == LoggerStatus::Available,
           "initializes file logger");

    expect(write_log_record(static_cast<LogLevel>(255U), "test", "invalid level").status ==
               LoggerStatus::Unavailable,
           "rejects unknown log record level");
    expect(write_log_record(LogLevel::Info, "test", "first record",
                            {{"station_id", "station-test"}}).status ==
               LoggerStatus::Available,
           "writes structured record");
    expect(write_log_record(LogLevel::Debug, "test", "secret check",
                            {{"password", "do-not-store"},
                             {"access_token", "also-secret"}}).status ==
               LoggerStatus::Available,
           "writes record with sensitive fields");
    expect(flush_log().status == LoggerStatus::Available,
           "flushes records before inspection");
    const std::string current = read_file(base);
    const std::string archived = read_file(base + ".1");
    expect(current.find("\"level\":\"INFO\"") != std::string::npos ||
               archived.find("\"level\":\"INFO\"") != std::string::npos,
           "record contains level");
    expect(current.find("\"station_id\":\"station-test\"") != std::string::npos ||
               archived.find("\"station_id\":\"station-test\"") != std::string::npos,
           "record contains non-sensitive field");
    expect(current.find("do-not-store") == std::string::npos &&
               current.find("also-secret") == std::string::npos &&
               archived.find("do-not-store") == std::string::npos &&
               archived.find("also-secret") == std::string::npos &&
               (current.find("[REDACTED]") != std::string::npos ||
                archived.find("[REDACTED]") != std::string::npos),
           "sensitive field values are redacted");

    expect(write_log_record(LogLevel::Info, "test", "record to rotate").status ==
               LoggerStatus::Available,
           "writes record after automatic rotation");
    expect(access((base + ".1").c_str(), F_OK) == 0,
           "automatic rotation creates first archive");
    expect(flush_log().status == LoggerStatus::Available,
           "flushes log sinks");
    expect(shutdown_logger().status == LoggerStatus::Available,
           "shuts down logger");

    LoggerConfig stderr_only{};
    stderr_only.minimum_level = LogLevel::Error;
    stderr_only.write_to_stderr = true;
    expect(initialize_logger(stderr_only).status == LoggerStatus::Available,
           "supports stderr-only configuration");
    expect(write_log_record(LogLevel::Debug, "test", "filtered").status ==
               LoggerStatus::Available,
           "filters records below minimum level");
    expect(shutdown_logger().status == LoggerStatus::Available,
           "shuts down stderr-only logger");

    const std::string removed_path = base + ".removed";
    std::remove(removed_path.c_str());
    LoggerConfig removed_file_config{};
    removed_file_config.file_path = removed_path;
    expect(initialize_logger(removed_file_config).status == LoggerStatus::Available,
           "initializes logger for deleted-file handling test");
    expect(std::remove(removed_path.c_str()) == 0,
           "removes configured log path while logger remains open");
    expect(write_log_record(LogLevel::Error, "test", "must not disappear").status ==
               LoggerStatus::Unavailable,
           "reports unavailable when configured log path disappears");
    expect(shutdown_logger().status == LoggerStatus::Available,
           "shuts down after configured log path disappears");

    std::remove(base.c_str());
    std::remove((base + ".1").c_str());
    std::remove((base + ".2").c_str());

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
