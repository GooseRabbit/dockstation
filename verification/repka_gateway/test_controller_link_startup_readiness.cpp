#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "hardware/interfaces/controller_link_startup_readiness.hpp"
#include "hardware/interfaces/controller_link_peer_session.hpp"

using namespace dockstation::hardware::interfaces;

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

ControllerLinkStartupReadinessInput ready_input() {
    ControllerLinkStartupReadinessInput input{};
    input.hardware_profile_confirmed = true;
    input.uart_configuration_valid = true;
    input.pair_credentials_available = true;
    input.instance_identity_generated = true;
    input.instance_identity_configuration_matches = true;
    input.protocol_version_major = 1U;
    input.local_controller_id = "esp32-controller-01";
    input.peer_controller_id = "repka-controller-01";
    return input;
}

bool blocked_by(
    const ControllerLinkStartupReadiness& result,
    ControllerLinkStartupBlocker blocker) {
    return result.is_blocked_by(blocker);
}
}  // namespace

int main() {
    const ControllerLinkStartupReadiness not_evaluated{};
    expect(!not_evaluated.is_ready(),
           "a default, unevaluated readiness result is fail-closed");

    const auto empty = evaluate_controller_link_startup_readiness({});
    expect(!empty.is_ready(), "empty startup input is never ready");
    expect(blocked_by(empty, ControllerLinkStartupBlocker::HardwareProfileNotConfirmed) &&
               blocked_by(empty, ControllerLinkStartupBlocker::UartConfigurationInvalid) &&
               blocked_by(empty, ControllerLinkStartupBlocker::ProtocolVersionInvalid) &&
               blocked_by(empty, ControllerLinkStartupBlocker::PairCredentialsUnavailable) &&
               blocked_by(empty, ControllerLinkStartupBlocker::InstanceIdentityUnavailable) &&
               blocked_by(empty, ControllerLinkStartupBlocker::InstanceIdentityConfigurationMismatch) &&
               blocked_by(empty, ControllerLinkStartupBlocker::LocalControllerIdMissing) &&
               blocked_by(empty, ControllerLinkStartupBlocker::PeerControllerIdMissing),
           "empty input reports every independently missing prerequisite");

    auto input = ready_input();
    auto result = evaluate_controller_link_startup_readiness(input);
    expect(result.is_ready() && result.blockers == 0U,
           "complete distinct valid identities and prerequisites are ready");

    input.hardware_profile_confirmed = false;
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::HardwareProfileNotConfirmed) &&
               !blocked_by(result, ControllerLinkStartupBlocker::UartConfigurationInvalid),
           "unconfirmed hardware profile blocks startup independently");
    input = ready_input();

    input.uart_configuration_valid = false;
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::UartConfigurationInvalid),
           "invalid UART profile blocks startup");
    input = ready_input();

    input.pair_credentials_available = false;
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::PairCredentialsUnavailable),
           "unavailable pair credentials block startup");
    input = ready_input();

    input.instance_identity_generated = false;
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::InstanceIdentityUnavailable),
           "missing per-boot instance identity blocks startup");
    input = ready_input();

    input.instance_identity_configuration_matches = false;
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(
               result,
               ControllerLinkStartupBlocker::InstanceIdentityConfigurationMismatch),
           "a stale configured local instance ID is rejected");
    input = ready_input();

    input.protocol_version_major = 0U;
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::ProtocolVersionInvalid),
           "a zero major protocol version is rejected before UART startup");
    input = ready_input();

    input.local_controller_id = {};
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::LocalControllerIdMissing),
           "missing local stable controller ID is reported");
    input = ready_input();

    input.peer_controller_id = {};
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::PeerControllerIdMissing),
           "missing peer stable controller ID is reported");
    input = ready_input();

    input.local_controller_id = std::string_view{"bad\0id", 6U};
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::LocalControllerIdInvalid),
           "embedded NUL in local controller ID is rejected");
    input = ready_input();

    input.peer_controller_id = std::string_view{"\xC0\xAF", 2U};
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::PeerControllerIdInvalid),
           "malformed UTF-8 peer controller ID is rejected");
    input = ready_input();

    const std::string oversized_id(
        kControllerLinkPeerSessionMaximumIdentifierBytes + 1U, 'x');
    input.peer_controller_id = oversized_id;
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::PeerControllerIdInvalid),
           "overlong peer controller ID is rejected before runtime creation");
    input = ready_input();

    input.local_controller_id = input.peer_controller_id;
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::ControllerIdsNotDistinct),
           "the local and remote stable controller IDs must be distinct");
    input = ready_input();

    input.local_controller_id = "контроллер-esp32";
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::LocalControllerIdInvalid),
           "non-ASCII stable controller IDs are rejected consistently on both platforms");

    input = ready_input();
    input.hardware_profile_confirmed = false;
    input.uart_configuration_valid = false;
    input.pair_credentials_available = false;
    input.local_controller_id = {};
    result = evaluate_controller_link_startup_readiness(input);
    expect(blocked_by(result, ControllerLinkStartupBlocker::HardwareProfileNotConfirmed) &&
               blocked_by(result, ControllerLinkStartupBlocker::UartConfigurationInvalid) &&
               blocked_by(result, ControllerLinkStartupBlocker::PairCredentialsUnavailable) &&
               blocked_by(result, ControllerLinkStartupBlocker::LocalControllerIdMissing) &&
               !result.is_ready(),
           "multiple independent blockers accumulate without masking one another");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
