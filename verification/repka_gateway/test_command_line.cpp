#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "platform/linux/command_line.hpp"

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

CommandLineParseResult parse(const std::vector<std::string>& arguments) {
    std::vector<const char*> values;
    values.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        values.push_back(argument.c_str());
    }
    return parse_command_line(static_cast<int>(values.size()), values.data());
}
}  // namespace

int main() {
    auto result = parse({"dockstation-gateway"});
    expect(result.status == CommandLineParseStatus::Success &&
               result.config.api_port == 8080U &&
               result.config.logger.write_to_stderr,
           "accepts defaults without additional arguments");

    result = parse({"dockstation-gateway", "--api-port", "8081",
                    "--log-level", "debug", "--log-file", "/tmp/gateway.jsonl",
                    "--no-log-stderr", "--log-max-file-size-bytes", "2048",
                    "--log-max-rotated-files", "4"});
    expect(result.status == CommandLineParseStatus::Success &&
               result.config.api_port == 8081U &&
               result.config.logger.minimum_level == logging::LogLevel::Debug &&
               result.config.logger.file_path == "/tmp/gateway.jsonl" &&
               !result.config.logger.write_to_stderr &&
               result.config.logger.max_file_size_bytes == 2048U &&
               result.config.logger.max_rotated_files == 4U,
           "parses all supported RuntimeConfig options");

    expect(parse({"dockstation-gateway", "--api-port", "0"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects API port zero");
    expect(parse({"dockstation-gateway", "--api-port", "65536"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects API port above 65535");
    expect(parse({"dockstation-gateway", "--api-port"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects option without a value");
    expect(parse({"dockstation-gateway", "--unknown"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects unknown option");
    expect(parse({"dockstation-gateway", "--log-level", "trace"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects unsupported log level");
    expect(parse({"dockstation-gateway", "--log-file", ""}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects empty log file path");
    expect(parse({"dockstation-gateway", "--no-log-stderr"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects configuration without any log sink");
    expect(parse({"dockstation-gateway", "--log-file", "/tmp/gateway.jsonl",
                  "--log-max-file-size-bytes", "0"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects zero file size when file sink is enabled");
    expect(parse({"dockstation-gateway", "--log-max-rotated-files", "21"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects more than 20 log archives");
    expect(parse({"dockstation-gateway", "--api-port", "8080",
                  "--api-port", "8081"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects duplicate options");
    expect(parse({"dockstation-gateway", "--log-stderr",
                  "--no-log-stderr"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects conflicting stderr options");
    expect(parse({"dockstation-gateway", "--help"}).status ==
               CommandLineParseStatus::HelpRequested,
           "recognizes standalone help request");
    expect(parse({"dockstation-gateway", "--help", "--api-port", "8080"}).status ==
               CommandLineParseStatus::InvalidArguments,
           "rejects help mixed with other options");

    const std::string help = command_line_help("dockstation-gateway");
    expect(help.find("--api-port") != std::string::npos &&
               help.find("127.0.0.1") != std::string::npos &&
               help.find("конфигурации") != std::string::npos,
           "help describes supported options and fixed loopback binding");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
