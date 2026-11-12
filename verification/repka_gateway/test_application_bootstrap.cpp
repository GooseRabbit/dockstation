#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

#include "application/linux/application_bootstrap.hpp"
#include "platform/logging/logger.hpp"

using namespace dockstation::application::linux_app;

namespace {
int failures = 0;
int checks = 0;
int runner_calls = 0;
int runner_result = 0;
bool runner_received_expected_config = false;
bool runner_raise_signal = false;
bool runner_saw_shutdown_signal = false;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

ApplicationRunResult run(const std::vector<std::string>& arguments,
                         ServiceRunner runner = nullptr) {
    std::vector<const char*> values;
    values.reserve(arguments.size());
    for (const auto& argument : arguments) {
        values.push_back(argument.c_str());
    }
    return run_application(static_cast<int>(values.size()), values.data(), runner);
}

int test_service_runner(
    const dockstation::platform::linux_runtime::RuntimeConfig& config,
    const volatile std::sig_atomic_t* shutdown_flag) {
    ++runner_calls;
    runner_received_expected_config =
        config.api_port == 8091U && shutdown_flag != nullptr &&
        !is_shutdown_requested(shutdown_flag);
    if (runner_raise_signal) {
        (void)std::raise(SIGTERM);
        runner_saw_shutdown_signal = is_shutdown_requested(shutdown_flag);
    }
    return runner_result;
}
}  // namespace

int main() {
    auto result = run({"dockstation-bootstrap", "--help"});
    expect(result.exit_code == ApplicationExitCode::Success &&
               result.help_requested,
           "help exits successfully without initializing runtime services");
    expect(dockstation::platform::logging::write_log_record(
               dockstation::platform::logging::LogLevel::Info,
               "bootstrap_test", "must not be initialized").status ==
               dockstation::platform::logging::LoggerStatus::Unavailable,
           "help does not initialize PlatformLogger");

    result = run({"dockstation-bootstrap", "--unknown"});
    expect(result.exit_code == ApplicationExitCode::InvalidArguments,
           "invalid command-line arguments fail before logger initialization");
    runner_calls = 0;
    result = run({"dockstation-bootstrap", "--api-port", "0"},
                 test_service_runner);
    expect(result.exit_code == ApplicationExitCode::InvalidArguments &&
               runner_calls == 0,
           "invalid runtime settings prevent service startup");

    const std::string log_path =
        "/tmp/dockstation_bootstrap_" +
        std::to_string(static_cast<long long>(::getpid())) + ".jsonl";
    std::remove(log_path.c_str());

    runner_calls = 0;
    runner_result = 0;
    runner_received_expected_config = false;
    result = run({"dockstation-bootstrap", "--api-port", "8091",
                  "--log-file", log_path, "--no-log-stderr"},
                 test_service_runner);
    expect(result.exit_code == ApplicationExitCode::Success,
           "runs startup, service callback, and orderly shutdown");
    expect(runner_calls == 1 && runner_received_expected_config,
           "passes validated configuration and shutdown flag to service runner");

    struct sigaction previous_sigint {};
    struct sigaction previous_sigterm {};
    const bool captured_previous_sigint =
        ::sigaction(SIGINT, nullptr, &previous_sigint) == 0;
    const bool captured_previous_sigterm =
        ::sigaction(SIGTERM, nullptr, &previous_sigterm) == 0;

    runner_raise_signal = true;
    runner_saw_shutdown_signal = false;
    result = run({"dockstation-bootstrap", "--api-port", "8091"},
                 test_service_runner);
    runner_raise_signal = false;

    struct sigaction restored_sigint {};
    struct sigaction restored_sigterm {};
    const bool captured_restored_sigint =
        ::sigaction(SIGINT, nullptr, &restored_sigint) == 0;
    const bool captured_restored_sigterm =
        ::sigaction(SIGTERM, nullptr, &restored_sigterm) == 0;
    expect(result.exit_code == ApplicationExitCode::Success &&
               runner_saw_shutdown_signal,
           "SIGTERM requests orderly service shutdown through sig_atomic_t");
    expect(captured_previous_sigint && captured_previous_sigterm &&
               captured_restored_sigint && captured_restored_sigterm &&
               restored_sigint.sa_handler == previous_sigint.sa_handler &&
               restored_sigterm.sa_handler == previous_sigterm.sa_handler,
           "restores the previous SIGINT and SIGTERM handlers");

    std::ifstream input(log_path);
    const std::string log_content(std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>());
    expect(log_content.find("Запуск Linux-приложения DockStation") !=
               std::string::npos &&
               log_content.find("Завершение Linux-приложения DockStation") !=
               std::string::npos,
           "records startup and shutdown in the configured log");
    expect(log_content.find("System diagnostic") != std::string::npos &&
               log_content.find("Network diagnostic list") != std::string::npos,
           "collects and logs read-only system and network diagnostics");
    std::remove(log_path.c_str());

    runner_calls = 0;
    runner_result = 0;
    result = run({"dockstation-bootstrap", "--log-file",
                  "/dev/null/dockstation_bootstrap.jsonl"},
                 test_service_runner);
    expect(result.exit_code == ApplicationExitCode::StartupFailure &&
               runner_calls == 0,
           "fails startup and does not start a service when the log sink cannot be opened");

    runner_calls = 0;
    runner_raise_signal = false;
    runner_result = 17;
    result = run({"dockstation-bootstrap"}, test_service_runner);
    expect(result.exit_code == ApplicationExitCode::ServiceFailure &&
               runner_calls == 1,
           "reports a non-zero service result as service failure");

    runner_calls = 0;
    result = run({"dockstation-bootstrap"});
    expect(result.exit_code == ApplicationExitCode::Success &&
               runner_calls == 0 &&
               result.message.find("HTTP API не запущен") != std::string::npos,
           "bootstrap-only mode explicitly reports that no HTTP API is running");

    volatile std::sig_atomic_t requested = 1;
    expect(is_shutdown_requested(&requested),
           "recognizes a requested shutdown flag");
    expect(!is_shutdown_requested(nullptr),
           "handles a null shutdown flag safely");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
