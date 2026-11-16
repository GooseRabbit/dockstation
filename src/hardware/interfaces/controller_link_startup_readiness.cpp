#include "hardware/interfaces/controller_link_startup_readiness.hpp"

#include "hardware/interfaces/controller_codec.hpp"
#include "hardware/interfaces/controller_stable_identity.hpp"
#include "hardware/interfaces/controller_link_peer_session.hpp"

namespace dockstation::hardware::interfaces {
namespace {

void add_blocker(
    ControllerLinkStartupReadiness& result,
    ControllerLinkStartupBlocker blocker) noexcept {
    result.blockers |= static_cast<std::uint32_t>(blocker);
}

}  // namespace

ControllerLinkStartupReadiness evaluate_controller_link_startup_readiness(
    const ControllerLinkStartupReadinessInput& input) noexcept {
    ControllerLinkStartupReadiness result{};
    result.evaluated = true;

    if (!input.hardware_profile_confirmed) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::HardwareProfileNotConfirmed);
    }
    if (!input.uart_configuration_valid) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::UartConfigurationInvalid);
    }
    if (!input.pair_credentials_available) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::PairCredentialsUnavailable);
    }
    if (!input.instance_identity_generated) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::InstanceIdentityUnavailable);
    }

    if (!input.instance_identity_configuration_matches) {
        add_blocker(
            result,
            ControllerLinkStartupBlocker::InstanceIdentityConfigurationMismatch);
    }

    if (input.protocol_version_major == 0U) {
        add_blocker(result, ControllerLinkStartupBlocker::ProtocolVersionInvalid);
    }

    const bool local_id_valid =
        is_valid_controller_stable_id(input.local_controller_id);
    if (input.local_controller_id.empty()) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::LocalControllerIdMissing);
    } else if (!local_id_valid) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::LocalControllerIdInvalid);
    }

    const bool peer_id_valid =
        is_valid_controller_stable_id(input.peer_controller_id);
    if (input.peer_controller_id.empty()) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::PeerControllerIdMissing);
    } else if (!peer_id_valid) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::PeerControllerIdInvalid);
    }

    if (local_id_valid && peer_id_valid &&
        input.local_controller_id == input.peer_controller_id) {
        add_blocker(result,
                    ControllerLinkStartupBlocker::ControllerIdsNotDistinct);
    }

    return result;
}

}  // namespace dockstation::hardware::interfaces
