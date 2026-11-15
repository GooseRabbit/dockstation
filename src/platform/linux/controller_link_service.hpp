#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "hardware/interfaces/controller_frame_stream_decoder.hpp"
#include "hardware/interfaces/controller_link_session.hpp"
#include "platform/linux/controller_transport_manager.hpp"

namespace dockstation::platform::linux_platform {

enum class ControllerLinkServiceState : std::uint8_t {
    NotConfigured,
    TlsHandshake,
    ProtocolHandshake,
    Established,
    LinkLost,
    Error,
    Stopped
};

struct ControllerLinkServiceConfig final {
    std::string_view uart_device_path{};
    std::string_view local_controller_id{};
    std::string_view peer_controller_id{};
};

struct ControllerLinkServiceStatus final {
    ControllerLinkServiceState state{ControllerLinkServiceState::NotConfigured};
    bool tls_authenticated{false};
    bool protocol_established{false};
    std::uint64_t telemetry_snapshots_received{0U};
    std::uint64_t error_messages_received{0U};
    std::uint64_t rejected_frames{0U};
    const char* error_code{""};
};

// Connects the Repka UART/TLS transport to the shared ControllerCodec and
// ControllerLinkSession. It deliberately exposes no physical-command API:
// the only initial application request is REQUEST_SNAPSHOT.
class ControllerLinkService final {
public:
    ControllerLinkService() noexcept = default;
    ~ControllerLinkService();

    ControllerLinkService(const ControllerLinkService&) = delete;
    ControllerLinkService& operator=(const ControllerLinkService&) = delete;
    ControllerLinkService(ControllerLinkService&&) = delete;
    ControllerLinkService& operator=(ControllerLinkService&&) = delete;

    // Missing optional settings or a not-yet-provisioned PSK leave the service
    // NOT_CONFIGURED without preventing the read-only Gateway from starting.
    [[nodiscard]] ControllerLinkServiceStatus start(
        const ControllerLinkServiceConfig& config) noexcept;

    // Bounded event-loop step. It advances TLS, performs HELLO/HELLO_ACK,
    // requests one telemetry snapshot per logical session, handles heartbeat,
    // and schedules a fresh TLS connection after UART/TLS failure.
    void poll() noexcept;
    void stop() noexcept;

    [[nodiscard]] ControllerLinkServiceStatus status() const noexcept;

private:
    [[nodiscard]] bool copy_configuration(
        const ControllerLinkServiceConfig& config) noexcept;
    [[nodiscard]] bool create_session() noexcept;
    void attempt_transport_start(std::uint64_t now_ms) noexcept;
    void fail_transport(const char* error_code, std::uint64_t now_ms) noexcept;
    [[nodiscard]] bool queue_outbound_message(
        const hardware::interfaces::ControllerMessageEnvelope& envelope) noexcept;
    [[nodiscard]] bool flush_outbound_message() noexcept;
    [[nodiscard]] bool process_plaintext(
        const std::uint8_t* bytes, std::size_t byte_count,
        std::uint64_t now_ms) noexcept;
    [[nodiscard]] bool process_ready_frame(std::uint64_t now_ms) noexcept;
    [[nodiscard]] bool begin_protocol_session(
        std::uint64_t now_ms, std::string_view sent_at) noexcept;
    void update_protocol_state() noexcept;

    ControllerTransportManager transport_{};
    hardware::interfaces::ControllerFrameStreamDecoder decoder_{};
    std::optional<hardware::interfaces::ControllerLinkSession> session_{};
    std::array<char, 256U> uart_device_path_{};
    std::array<char, hardware::interfaces::kControllerLinkSessionMaximumIdentifierBytes>
        local_controller_id_{};
    std::array<char, hardware::interfaces::kControllerLinkSessionMaximumIdentifierBytes>
        peer_controller_id_{};
    std::array<char, 32U> local_instance_id_{};
    std::array<std::uint8_t,
               hardware::interfaces::kControllerCodecMaximumFrameSize> outbound_frame_{};
    std::size_t uart_device_path_size_{0U};
    std::size_t local_controller_id_size_{0U};
    std::size_t peer_controller_id_size_{0U};
    std::size_t outbound_frame_size_{0U};
    std::size_t outbound_frame_offset_{0U};
    std::uint64_t next_retry_ms_{0U};
    std::uint64_t telemetry_snapshots_received_{0U};
    std::uint64_t error_messages_received_{0U};
    std::uint64_t rejected_frames_{0U};
    ControllerLinkServiceState state_{ControllerLinkServiceState::NotConfigured};
    const char* error_code_{""};
    bool configured_{false};
    bool tls_authenticated_{false};
    bool snapshot_request_pending_{false};
};

}  // namespace dockstation::platform::linux_platform
