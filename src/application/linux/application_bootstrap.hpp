#pragma once

#include <csignal>
#include <string>

#include "platform/linux/runtime_config.hpp"

namespace dockstation::application::linux_app {

enum class ApplicationExitCode : int {
    Success = 0,
    InvalidArguments = 2,
    StartupFailure = 3,
    ServiceFailure = 4,
    ShutdownFailure = 5
};

// A service runner must return when shutdown_requested becomes non-zero.
// It must not perform physical actions merely because the bootstrap started.
using ServiceRunner = int (*)(
    const platform::linux_runtime::RuntimeConfig& config,
    const volatile std::sig_atomic_t* shutdown_requested);

struct ApplicationRunResult {
    ApplicationExitCode exit_code{ApplicationExitCode::StartupFailure};
    bool help_requested{false};
    std::string message{};
};

// Runs process startup, optional service execution, and orderly shutdown.
// A null service_runner performs a bootstrap-only startup check and exits;
// it does not start an HTTP server or claim that Gateway is serving requests.
[[nodiscard]] ApplicationRunResult run_application(
    int argc,
    const char* const argv[],
    ServiceRunner service_runner = nullptr);

// Safe to read from a service loop; the signal handler only writes sig_atomic_t.
[[nodiscard]] bool is_shutdown_requested(
    const volatile std::sig_atomic_t* shutdown_requested) noexcept;

}  // namespace dockstation::application::linux_app
