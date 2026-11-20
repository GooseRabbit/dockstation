#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "hardware/interfaces/controller_frame_stream_decoder.hpp"
#include "hardware/interfaces/controller_link_session.hpp"
#include "hardware/interfaces/controller_telemetry_snapshot.hpp"
#include "platform/linux/controller_psk_credential_store.hpp"
#include "platform/linux/controller_transport_manager.hpp"

namespace dockstation::platform::linux_platform {

class ControllerLinkService;

// Synchronous sink for application messages accepted by both TLS and the
// ControllerLink session state machine. The decoded views are valid only for
// the duration of this callback.
class ControllerLinkMessageHandler {
public:
    virtual ~ControllerLinkMessageHandler() = default;
    virtual void on_authenticated_controller_message(
        ControllerLinkService& service,
        const hardware::interfaces::ControllerCodecDecodedMessage& message,
        std::uint64_t received_at_unix_ms) noexcept = 0;
};

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

// Connects the Repka UART/TLS transport to ControllerCodec and
// ControllerLinkSession. REQUEST_SNAPSHOT is the autonomous telemetry request;
// request_command() is a narrow transport seam for an already-authorized caller.
// It does not execute hardware actions or publish command completion to the API.
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
    // requests telemetry immediately after session establishment and every
    // two seconds while established, handles heartbeat, and reconnects after
    // UART/TLS failure.
    void poll() noexcept;
    void stop() noexcept;

    [[nodiscard]] ControllerLinkServiceStatus status() const noexcept;

    // Attach before start(); this service must outlive the handler, which
    // detaches itself before destruction. A single handler is supported.
    [[nodiscard]] bool attach_message_handler(
        ControllerLinkMessageHandler& handler) noexcept;
    void detach_message_handler(
        ControllerLinkMessageHandler& handler) noexcept;

    // Requests a station command only after both authenticated sessions are
    // established. This is a transport seam: callers must obtain safety
    // permission before invoking it; this method does not bypass SafetyManager.
    [[nodiscard]] bool request_command(
        hardware::interfaces::ControllerCommandType command) noexcept;

    // These values are available only while the authenticated TLS and
    // ControllerLink sessions are both Established. The pair identity is
    // copied from the local credential actually used by the TLS client.
    [[nodiscard]] std::string_view authenticated_pair_identity() const noexcept;
    [[nodiscard]] std::string_view authenticated_peer_controller_id() const noexcept;

    [[nodiscard]] const hardware::interfaces::ControllerTelemetrySnapshot&
    telemetry_snapshot() const noexcept { return telemetry_snapshot_; }

    // Available only after TLS authentication and the ControllerLink HELLO_ACK.
    // The fixed non-empty context is versioned with the DockStation protocol.
    [[nodiscard]] bool export_authenticated_session_proof(
        std::array<std::uint8_t, kControllerSessionProofSize>& output) const noexcept;

private:
#ifdef DOCKSTATION_ENABLE_TEST_SEAMS
    // Present only in CMake verification builds; release Gateway builds always
    // call the protected on-disk credential loader directly.
    friend class ControllerLinkServiceTestAccess;
    decltype(&load_controller_psk_credentials) credential_loader_{
        &load_controller_psk_credentials};
#endif

    [[nodiscard]] bool copy_configuration(
        const ControllerLinkServiceConfig& config) noexcept;
    [[nodiscard]] bool create_session() noexcept;
    void attempt_transport_start(std::uint64_t now_ms) noexcept;
    void fail_transport(const char* error_code, std::uint64_t now_ms) noexcept;
    [[nodiscard]] bool queue_outbound_message(
        const hardware::interfaces::ControllerMessageEnvelope& envelope,
        const hardware::interfaces::ControllerMessagePayload& payload) noexcept;
    [[nodiscard]] bool flush_outbound_message() noexcept;
    [[nodiscard]] bool process_plaintext(
        const std::uint8_t* bytes, std::size_t byte_count,
        std::uint64_t now_ms) noexcept;
    [[nodiscard]] bool process_ready_frame(std::uint64_t now_ms) noexcept;
    [[nodiscard]] bool begin_protocol_session(
        std::uint64_t now_ms, std::string_view sent_at) noexcept;
    void update_protocol_state() noexcept;
    void set_state(ControllerLinkServiceState state,
                   const char* error_code) noexcept;
    void log_controller_error(
        const hardware::interfaces::ControllerErrorPayload& error,
        std::uint64_t sequence_number) noexcept;

    ControllerTransportManager transport_{};
    hardware::interfaces::ControllerFrameStreamDecoder decoder_{};
    std::optional<hardware::interfaces::ControllerLinkSession> session_{};
    hardware::interfaces::ControllerTelemetrySnapshot telemetry_snapshot_{};
    std::array<char, 256U> uart_device_path_{};
    std::array<char, hardware::interfaces::kControllerLinkSessionMaximumIdentifierBytes>
        local_controller_id_{};
    std::array<char, hardware::interfaces::kControllerLinkSessionMaximumIdentifierBytes>
        peer_controller_id_{};
    std::array<char, 32U> local_instance_id_{};
    std::array<std::uint8_t,
               hardware::interfaces::kControllerCodecMaximumFrameSize> outbound_frame_{};
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
    std::uint64_t last_snapshot_request_ms_{0U};
    bool snapshot_request_sent_this_session_{false};
    ControllerLinkMessageHandler* message_handler_{nullptr};
    std::array<std::uint8_t, kControllerPskIdentitySize>
        authenticated_pair_identity_{};
    bool authenticated_pair_identity_available_{false};
    // The wire frame bounds the maximum peer error-code size. Repeated reports
    // with the same code are logged once per authenticated session.
    std::array<char, 2048U> last_reported_controller_error_code_{};
    std::size_t last_reported_controller_error_code_size_{0U};
};

}  // namespace dockstation::platform::linux_platform
