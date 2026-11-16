#pragma once

#include <cstdint>
#include <string_view>

namespace dockstation::hardware::interfaces {

enum class ControllerLinkStartupBlocker : std::uint32_t {
    None = 0U,
    HardwareProfileNotConfirmed = 1U << 0U,
    UartConfigurationInvalid = 1U << 1U,
    PairCredentialsUnavailable = 1U << 2U,
    InstanceIdentityUnavailable = 1U << 3U,
    InstanceIdentityConfigurationMismatch = 1U << 4U,
    LocalControllerIdMissing = 1U << 5U,
    LocalControllerIdInvalid = 1U << 6U,
    PeerControllerIdMissing = 1U << 7U,
    PeerControllerIdInvalid = 1U << 8U,
    ControllerIdsNotDistinct = 1U << 9U,
    ProtocolVersionInvalid = 1U << 10U
};

struct ControllerLinkStartupReadinessInput final {
    bool hardware_profile_confirmed{false};
    bool uart_configuration_valid{false};
    bool pair_credentials_available{false};
    bool instance_identity_generated{false};
    bool instance_identity_configuration_matches{false};
    std::uint16_t protocol_version_major{0U};
    std::string_view local_controller_id{};
    std::string_view peer_controller_id{};
};

struct ControllerLinkStartupReadiness final {
    bool evaluated{false};
    std::uint32_t blockers{0U};

    // An unevaluated/default result is not permission to start hardware.
    [[nodiscard]] bool is_ready() const noexcept {
        return evaluated && blockers == 0U;
    }

    [[nodiscard]] bool is_blocked_by(
        ControllerLinkStartupBlocker blocker) const noexcept {
        return (blockers & static_cast<std::uint32_t>(blocker)) != 0U;
    }
};

// Pure, side-effect-free gate shared by firmware and host verification.
// It deliberately does not load credentials, probe GPIO or open a transport.
[[nodiscard]] ControllerLinkStartupReadiness
evaluate_controller_link_startup_readiness(
    const ControllerLinkStartupReadinessInput& input) noexcept;

}  // namespace dockstation::hardware::interfaces
