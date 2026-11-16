#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hardware/interfaces/controller_codec.hpp"

namespace dockstation::hardware::interfaces {

inline constexpr std::size_t kControllerLinkPeerSessionMaximumIdentifierBytes =
    128U;

enum class ControllerLinkPeerSessionState : std::uint8_t {
    Stopped,
    AwaitingHello,
    Established
};

enum class ControllerLinkPeerSessionStatus : std::uint8_t {
    Success,
    InvalidConfiguration,
    InvalidTimestamp,
    InvalidState,
    InvalidMessage,
    UnexpectedMessage,
    PeerIdentityMismatch,
    PeerInstanceMismatch,
    ProtocolVersionMismatch,
    SequenceNotIncreasing,
    IdentifierTooLong,
    SequenceExhausted
};

enum class ControllerLinkPeerSessionEvent : std::uint8_t {
    None,
    SessionEstablished,
    PeerHeartbeatReceived,
    SnapshotRequested
};

struct ControllerLinkPeerSessionConfig final {
    std::string_view local_controller_id{};
    // Generated once for this controller boot and retained for this object.
    std::string_view local_controller_instance_id{};
    std::string_view peer_controller_id{};
    ControllerProtocolVersion protocol_version{1U, 1U};
};

struct ControllerLinkPeerSessionResult final {
    ControllerLinkPeerSessionStatus status{
        ControllerLinkPeerSessionStatus::Success};
    ControllerLinkPeerSessionEvent event{
        ControllerLinkPeerSessionEvent::None};
    ControllerLinkPeerSessionState state{
        ControllerLinkPeerSessionState::Stopped};
    bool has_outbound_message{false};
    // Envelope identifiers refer to this session's fixed storage. sent_at
    // refers to the caller's string and must remain alive until encoding ends.
    ControllerMessageEnvelope outbound_message{};
    // Any string views and measurement storage in this payload remain owned by
    // the caller. Encode the message before changing or releasing that storage.
    ControllerMessagePayload outbound_payload{EmptyControllerMessagePayload{}};
};

// Logical peer-side session for the ESP32/controller end of ControllerLink.
// The caller must start it only after the transport has authenticated the
// Repka peer (for example, after a successful TLS PSK handshake). This class
// performs protocol validation only; it does not provide TLS, UART or sensors.
class ControllerLinkPeerSession final {
public:
    explicit ControllerLinkPeerSession(
        const ControllerLinkPeerSessionConfig& config) noexcept;

    // A session owns transmit and replay counters. Copying or moving it could
    // duplicate sequence history or invalidate outbound identifier views.
    ControllerLinkPeerSession(const ControllerLinkPeerSession&) = delete;
    ControllerLinkPeerSession& operator=(const ControllerLinkPeerSession&) = delete;
    ControllerLinkPeerSession(ControllerLinkPeerSession&&) = delete;
    ControllerLinkPeerSession& operator=(ControllerLinkPeerSession&&) = delete;

    [[nodiscard]] bool is_configured() const noexcept { return configured_; }
    [[nodiscard]] ControllerLinkPeerSessionState state() const noexcept {
        return state_;
    }

    // Begins protocol processing for an already authenticated transport.
    // Sequence and peer-instance history are retained across stop/start.
    [[nodiscard]] ControllerLinkPeerSessionResult start() noexcept;

    // Accepts HELLO while awaiting the first message and returns HELLO_ACK.
    // Once established, only HEARTBEAT and REQUEST_SNAPSHOT are accepted.
    [[nodiscard]] ControllerLinkPeerSessionResult handle_message(
        const ControllerCodecDecodedMessage& message,
        std::string_view sent_at) noexcept;

    // Builds a response to an accepted snapshot request. Sensor acquisition is
    // performed by the caller; this method only validates and sequences data.
    [[nodiscard]] ControllerLinkPeerSessionResult make_telemetry_snapshot(
        const ControllerTelemetrySnapshotPayload& snapshot,
        std::string_view sent_at) noexcept;

    // Ends the current logical session but preserves counters and peer replay
    // history so a reconnect on the same object cannot replay old messages.
    void stop() noexcept;

private:
    [[nodiscard]] ControllerLinkPeerSessionResult make_result(
        ControllerLinkPeerSessionStatus status,
        ControllerLinkPeerSessionEvent event) const noexcept;
    [[nodiscard]] ControllerLinkPeerSessionResult make_outbound(
        ControllerMessageType type,
        ControllerLinkPeerSessionEvent event,
        const ControllerMessagePayload& payload,
        std::string_view sent_at) noexcept;

    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes>
        local_controller_id_{};
    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes>
        local_controller_instance_id_{};
    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes>
        peer_controller_id_{};
    std::array<char, kControllerLinkPeerSessionMaximumIdentifierBytes>
        peer_controller_instance_id_{};
    std::size_t local_controller_id_size_{0U};
    std::size_t local_controller_instance_id_size_{0U};
    std::size_t peer_controller_id_size_{0U};
    std::size_t peer_controller_instance_id_size_{0U};

    ControllerProtocolVersion protocol_version_{1U, 1U};
    ControllerLinkPeerSessionState state_{
        ControllerLinkPeerSessionState::Stopped};
    std::uint64_t next_tx_sequence_{1U};
    std::uint64_t last_peer_sequence_{0U};
    bool configured_{false};
    bool tx_sequence_exhausted_{false};
    bool peer_sequence_known_{false};
};

}  // namespace dockstation::hardware::interfaces
