#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <optional>

#include "hardware/interfaces/controller_codec.hpp"

namespace dockstation::hardware::interfaces {

inline constexpr std::size_t kControllerLinkPeerSessionMaximumIdentifierBytes = 128U;

enum class ControllerLinkPeerSessionState : std::uint8_t { Stopped, AwaitingHello, Established };
enum class ControllerLinkPeerSessionStatus : std::uint8_t {
    Success, InvalidConfiguration, InvalidTimestamp, InvalidState, InvalidMessage,
    UnexpectedMessage, PeerIdentityMismatch, PeerInstanceMismatch,
    ProtocolVersionMismatch, SequenceNotIncreasing, IdentifierTooLong, SequenceExhausted
};
enum class ControllerLinkPeerSessionEvent : std::uint8_t {
    None, SessionEstablished, PeerHeartbeatReceived, SnapshotRequested, CommandRequested
};

struct ControllerLinkPeerSessionConfig final {
    std::string_view local_controller_id{};
    std::string_view local_controller_instance_id{};
    std::string_view peer_controller_id{};
    ControllerProtocolVersion protocol_version{1U, 1U};
};

struct ControllerLinkPeerSessionResult final {
    ControllerLinkPeerSessionStatus status{ControllerLinkPeerSessionStatus::Success};
    ControllerLinkPeerSessionEvent event{ControllerLinkPeerSessionEvent::None};
    ControllerLinkPeerSessionState state{ControllerLinkPeerSessionState::Stopped};
    bool has_outbound_message{false};
    ControllerMessageEnvelope outbound_message{};
    ControllerMessagePayload outbound_payload{EmptyControllerMessagePayload{}};
};

class ControllerLinkPeerSession final {
public:
    explicit ControllerLinkPeerSession(const ControllerLinkPeerSessionConfig& config) noexcept;
    ControllerLinkPeerSession(const ControllerLinkPeerSession&) = delete;
    ControllerLinkPeerSession& operator=(const ControllerLinkPeerSession&) = delete;
    ControllerLinkPeerSession(ControllerLinkPeerSession&&) = delete;
    ControllerLinkPeerSession& operator=(ControllerLinkPeerSession&&) = delete;

    [[nodiscard]] bool is_configured() const noexcept { return configured_; }
    [[nodiscard]] ControllerLinkPeerSessionState state() const noexcept { return state_; }
    [[nodiscard]] std::optional<ControllerCommandType> pending_command() const noexcept { return command_requested_; }
    [[nodiscard]] ControllerLinkPeerSessionResult start() noexcept;
    [[nodiscard]] ControllerLinkPeerSessionResult handle_message(
        const ControllerCodecDecodedMessage& message, std::string_view sent_at) noexcept;
    [[nodiscard]] ControllerLinkPeerSessionResult make_command_result(
        ControllerCommandType command, ControllerCommandResultStatus status,
        std::string_view reason_code, std::string_view sent_at) noexcept;
    [[nodiscard]] ControllerLinkPeerSessionResult make_telemetry_snapshot(
        const ControllerTelemetrySnapshotPayload& snapshot, std::string_view sent_at) noexcept;
    void stop() noexcept;

private:
    [[nodiscard]] ControllerLinkPeerSessionResult make_result(
        ControllerLinkPeerSessionStatus status,
        ControllerLinkPeerSessionEvent event) const noexcept;
    [[nodiscard]] ControllerLinkPeerSessionResult make_outbound(
        ControllerMessageType type, ControllerLinkPeerSessionEvent event,
        const ControllerMessagePayload& payload, std::string_view sent_at) noexcept;

    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes> local_controller_id_{};
    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes> local_controller_instance_id_{};
    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes> peer_controller_id_{};
    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes> peer_controller_instance_id_{};
    std::size_t local_controller_id_size_{0U};
    std::size_t local_controller_instance_id_size_{0U};
    std::size_t peer_controller_id_size_{0U};
    std::size_t peer_controller_instance_id_size_{0U};
    ControllerProtocolVersion protocol_version_{1U, 1U};
    ControllerLinkPeerSessionState state_{ControllerLinkPeerSessionState::Stopped};
    std::uint64_t next_tx_sequence_{1U};
    std::uint64_t last_peer_sequence_{0U};
    bool configured_{false};
    bool tx_sequence_exhausted_{false};
    bool peer_sequence_known_{false};
    bool snapshot_requested_{false};
    std::optional<ControllerCommandType> command_requested_{};
};

}  // namespace dockstation::hardware::interfaces
