#include <cstdlib>
#include <iostream>
#include <string>

#include "platform/linux/controller_link_service.hpp"

using namespace dockstation::platform::linux_platform;

namespace {
int checks = 0;
int failures = 0;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        std::cerr << "FAIL - " << description << '\n';
        ++failures;
    }
}
}  // namespace

int main() {
    ControllerLinkService service{};
    const auto not_configured = service.start({});
    expect(not_configured.state == ControllerLinkServiceState::NotConfigured &&
               !not_configured.tls_authenticated &&
               !not_configured.protocol_established,
           "an omitted optional controller link remains NOT_CONFIGURED");

    service.poll();
    expect(service.status().state == ControllerLinkServiceState::NotConfigured,
           "poll does not invent a link or hardware state when not configured");

    ControllerLinkServiceConfig partial{};
    partial.uart_device_path = "/dev/serial0";
    partial.local_controller_id = "repka-01";
    const auto partial_result = service.start(partial);
    expect(partial_result.state == ControllerLinkServiceState::Error &&
               partial_result.error_code != nullptr &&
               std::string(partial_result.error_code) ==
                   "CONTROLLER_CONFIGURATION_INVALID",
           "a partial controller identity configuration is rejected before device access");

    ControllerLinkServiceConfig invalid_path{};
    invalid_path.uart_device_path = "/tmp/tty";
    invalid_path.local_controller_id = "repka-01";
    invalid_path.peer_controller_id = "esp32-01";
    const auto path_result = service.start(invalid_path);
    expect(path_result.state == ControllerLinkServiceState::Error &&
               path_result.error_code != nullptr &&
               std::string(path_result.error_code) ==
                   "CONTROLLER_CONFIGURATION_INVALID",
           "a non-/dev UART path is rejected before credential or device access");

    service.stop();
    expect(service.status().state == ControllerLinkServiceState::Stopped,
           "stop clears optional controller link runtime state");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
