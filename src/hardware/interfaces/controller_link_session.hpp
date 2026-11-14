#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hardware/interfaces/controller_codec.hpp"

namespace dockstation::hardware::interfaces {

inline constexpr std::size_t kControllerLinkSessionMaximumIdentifierBytes = 128U;

enum class ControllerLinkSessionState : std::uint8_t {
    Stopped,
    AwaitingHelloAck,
    Established,
    LinkLost
};

enum class ControllerLinkSessionStatus : std::uint8_t {
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
    HandshakeTimeout,
    HeartbeatTimeout,
    SequenceExhausted
};

enum class ControllerLinkSessionEvent : std::uint8_t {
    None,
    HelloRequired,
    HeartbeatDue,
    SessionEstablished,
    PeerHeartbeatReceived,
    PeerMessageReceived,
    LinkLost
};

struct ControllerLinkSessionConfig {
    std::string_view local_controller_id{};
    std::string_view local_controller_instance_id{};
    std::string_view peer_controller_id{};
    ControllerProtocolVersion protocol_version{1U, 0U};
    std::uint64_t handshake_timeout_ms{3000U};
    std::uint64_t heartbeat_interval_ms{1000U};
    std::uint64_t heartbeat_timeout_ms{5000U};
    std::uint64_t retry_interval_ms{1000U};
};

struct ControllerLinkSessionResult {
    ControllerLinkSessionStatus status{ControllerLinkSessionStatus::Success};
    ControllerLinkSessionEvent event{ControllerLinkSessionEvent::None};
    ControllerLinkSessionState state{ControllerLinkSessionState::Stopped};
    bool has_outbound_message{false};
    // The envelope views refer to this session's fixed identifier storage and
    // to the caller-owned sent_at string. Encode the message before either
    // backing object is changed or destroyed. Outbound payloads are always
    // EmptyControllerMessagePayload.
    ControllerMessageEnvelope outbound_message{};
};

// Protocol-level Repka/controller session coordinator. It has no UART, timer,
// authentication, encryption, storage or hardware dependencies. Time is
// supplied by the caller as monotonic milliseconds.
class ControllerLinkSession final {
public:
    explicit ControllerLinkSession(
        const ControllerLinkSessionConfig& config) noexcept;

    [[nodiscard]] bool is_configured() const noexcept { return configured_; }
    [[nodiscard]] ControllerLinkSessionState state() const noexcept {
        return state_;
    }

    // Starts/restarts the host-initiated HELLO exchange. Sequence numbers
    // remain monotonic for the lifetime of this object, including retries.
    [[nodiscard]] ControllerLinkSessionResult start(
        std::uint64_t now_ms, std::string_view sent_at) noexcept;

    // Applies handshake/heartbeat deadlines, emits scheduled HEARTBEAT
    // envelopes, and retries HELLO after a lost link.
    [[nodiscard]] ControllerLinkSessionResult poll(
        std::uint64_t now_ms, std::string_view sent_at) noexcept;

    // Accepts only HELLO_ACK while handshaking, then HEARTBEAT,
    // TELEMETRY_SNAPSHOT or ERROR from the established peer.
    [[nodiscard]] ControllerLinkSessionResult handle_message(
        const ControllerCodecDecodedMessage& message,
        std::uint64_t now_ms) noexcept;

    // Stops the state machine but preserves sequence history and the last
    // observed peer instance/sequence to avoid accepting stale replayed frames
    // if this object is started again.
    void stop() noexcept;

private:
    [[nodiscard]] ControllerLinkSessionResult make_result(
        ControllerLinkSessionStatus status,
        ControllerLinkSessionEvent event) const noexcept;
    [[nodiscard]] ControllerLinkSessionResult make_outbound(
        ControllerMessageType type,
        ControllerLinkSessionEvent event,
        std::uint64_t now_ms,
        std::string_view sent_at) noexcept;

    std::array<char, kControllerLinkSessionMaximumIdentifierBytes>
        local_controller_id_{};
    std::array<char, kControllerLinkSessionMaximumIdentifierBytes>
        local_controller_instance_id_{};
    std::array<char, kControllerLinkSessionMaximumIdentifierBytes>
        peer_controller_id_{};
    std::array<char, kControllerLinkSessionMaximumIdentifierBytes>
        peer_controller_instance_id_{};
    std::size_t local_controller_id_size_{0U};
    std::size_t local_controller_instance_id_size_{0U};
    std::size_t peer_controller_id_size_{0U};
    std::size_t peer_controller_instance_id_size_{0U};

    ControllerProtocolVersion protocol_version_{1U, 0U};
    std::uint64_t handshake_timeout_ms_{0U};
    std::uint64_t heartbeat_interval_ms_{0U};
    std::uint64_t heartbeat_timeout_ms_{0U};
    std::uint64_t retry_interval_ms_{0U};

    ControllerLinkSessionState state_{ControllerLinkSessionState::Stopped};
    std::uint64_t next_tx_sequence_{1U};
    std::uint64_t last_outbound_message_ms_{0U};
    std::uint64_t last_peer_activity_ms_{0U};
    std::uint64_t last_retry_ms_{0U};
    std::uint64_t last_peer_sequence_{0U};
    bool configured_{false};
    bool tx_sequence_exhausted_{false};
    bool peer_sequence_known_{false};
};

}  // namespace dockstation::hardware::interfaces
