#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "controller_mbedtls_psk_server.hpp"
#include "hardware/interfaces/controller_frame_stream_decoder.hpp"
#include "hardware/interfaces/controller_link_peer_session.hpp"
#include "controller_station_state.hpp"

namespace dockstation::firmware::esp32 {

enum class ControllerLinkPeerServiceState : std::uint8_t {
    NotConfigured,
    TlsHandshake,
    AwaitingHello,
    Established,
    Error,
    Stopped
};

struct ControllerLinkPeerServiceStatus final {
    ControllerLinkPeerServiceState state{
        ControllerLinkPeerServiceState::NotConfigured};
    bool tls_authenticated{false};
    bool protocol_established{false};
    bool snapshot_request_pending{false};
    bool outbound_message_pending{false};
    std::uint64_t messages_received{0U};
    std::uint64_t telemetry_snapshots_sent{0U};
    std::uint64_t rejected_frames{0U};
    int last_mbedtls_error{0};
    hardware::interfaces::ControllerLinkPeerSessionStatus
        last_protocol_status{
            hardware::interfaces::ControllerLinkPeerSessionStatus::Success};
};

// Composes the ESP32 TLS server with shared ControllerLink frame, codec and
// peer-session logic. The caller supplies a byte-stream transport (UART in
// firmware), timestamp text, and real sensor readings for telemetry responses.
class ControllerLinkPeerService final {
public:
    ControllerLinkPeerService() noexcept = default;
    ~ControllerLinkPeerService();

    ControllerLinkPeerService(const ControllerLinkPeerService&) = delete;
    ControllerLinkPeerService& operator=(const ControllerLinkPeerService&) = delete;
    ControllerLinkPeerService(ControllerLinkPeerService&&) = delete;
    ControllerLinkPeerService& operator=(ControllerLinkPeerService&&) = delete;

    // First connection for this controller boot. Session IDs and instance are
    // retained for the lifetime of this object.
    [[nodiscard]] bool start(
        const ControllerMbedTlsPskCredentials& credentials,
        const hardware::interfaces::ControllerLinkPeerSessionConfig& session_config,
        void* transport_context,
        mbedtls_ssl_send_t* send_callback,
        mbedtls_ssl_recv_t* receive_callback) noexcept;

    // Restarts only the authenticated transport. The existing peer session
    // object and its sequence/replay history are preserved.
    [[nodiscard]] bool restart_transport(
        const ControllerMbedTlsPskCredentials& credentials,
        void* transport_context,
        mbedtls_ssl_send_t* send_callback,
        mbedtls_ssl_recv_t* receive_callback) noexcept;

    // One bounded event-loop step. sent_at is non-empty validated text used
    // when responding to HELLO with HELLO_ACK.
    void poll(std::string_view sent_at) noexcept;

    [[nodiscard]] bool snapshot_request_pending() const noexcept {
        return snapshot_request_pending_;
    }

    // Queues one caller-supplied snapshot only after an accepted
    // REQUEST_SNAPSHOT. Does not acquire sensors or fabricate measurements.
    [[nodiscard]] bool send_telemetry_snapshot(
        const hardware::interfaces::ControllerTelemetrySnapshotPayload& snapshot,
        std::string_view sent_at) noexcept;

    // Requires TLS authentication AND a fully transmitted HELLO_ACK.
    [[nodiscard]] bool export_authenticated_session_proof(
        std::array<std::uint8_t,
                   hardware::interfaces::kControllerLinkSessionProofSize>&
            output) noexcept;

    [[nodiscard]] ControllerLinkPeerServiceStatus status() const noexcept;
    void stop() noexcept;

private:
    [[nodiscard]] bool process_pending_plaintext(
        std::string_view sent_at) noexcept;
    [[nodiscard]] bool handle_command_request(std::string_view sent_at) noexcept;
    [[nodiscard]] bool process_ready_frame(
        std::string_view sent_at) noexcept;
    [[nodiscard]] bool queue_outbound_message(
        const hardware::interfaces::ControllerMessageEnvelope& envelope,
        const hardware::interfaces::ControllerMessagePayload& payload,
        bool is_hello_ack) noexcept;
    [[nodiscard]] bool flush_outbound_message() noexcept;
    void fail() noexcept;
    void clear_connection_buffers() noexcept;
    void begin_tls_handshake() noexcept;

    ControllerMbedTlsPskServer tls_server_{};
    std::optional<hardware::interfaces::ControllerLinkPeerSession> session_{};
    hardware::interfaces::ControllerFrameStreamDecoder decoder_{};
    hardware::interfaces::ControllerCodecDecodeStorage decode_storage_{};

    std::array<std::uint8_t, 1024U> inbound_buffer_{};
    std::size_t inbound_size_{0U};
    std::size_t inbound_offset_{0U};

    std::array<std::uint8_t,
               hardware::interfaces::kControllerCodecMaximumFrameSize>
        outbound_frame_{};
    std::size_t outbound_frame_size_{0U};
    std::size_t outbound_frame_offset_{0U};

    ControllerLinkPeerServiceState state_{
        ControllerLinkPeerServiceState::NotConfigured};
    hardware::interfaces::ControllerLinkPeerSessionStatus last_protocol_status_{
        hardware::interfaces::ControllerLinkPeerSessionStatus::Success};
    std::uint64_t messages_received_{0U};
    std::uint64_t telemetry_snapshots_sent_{0U};
    std::uint64_t rejected_frames_{0U};
    bool logical_handshake_ack_pending_{false};
    bool logical_session_established_{false};
    bool snapshot_request_pending_{false};
    ControllerStationState station_state_{};
    int last_mbedtls_error_{0};
};

}  // namespace dockstation::firmware::esp32
