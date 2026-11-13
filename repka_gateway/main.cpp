#include <cstddef>
#include <csignal>
#include <vector>

#include "application/linux/application_bootstrap.hpp"
#include "communications/local_api_server.hpp"
#include "platform/linux/runtime_config.hpp"

int main(int argc, char* argv[]) {
    std::vector<const char*> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(argv[index]);
    }

    const auto service_runner =
        [](const dockstation::platform::linux_runtime::RuntimeConfig& config,
           const volatile std::sig_atomic_t* shutdown_requested) {
            return dockstation::communications::run_local_api_server(
                config.api_port, shutdown_requested);
        };

    const auto result =
        dockstation::application::linux_app::run_application(
            argc, arguments.data(), service_runner);
    return static_cast<int>(result.exit_code);
}
