#include "controller_link_firmware_runtime.hpp"

namespace dockstation::firmware::esp32 {
namespace {

constexpr char kTag[] = "DockStationRuntime";
constexpr std::uint64_t kControllerHandshakeTimeoutUs = 3000000U;
constexpr std::uint64_t kControllerHeartbeatTimeoutUs = 5000000U;

[[nodiscard]] std::uint64_t monotonic_microseconds() noexcept {
    const std::int64_t value = esp_timer_get_time();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

[[nodiscard]] bool elapsed_at_least(
    std::uint64_t now_us, std::uint64_t since_us,
    std::uint64_t interval_us) noexcept {
    return now_us >= since_us && (now_us - since_us) >= interval_us;
}

template <std::size_t Size>
[[nodiscard]] bool contains_nonzero(
    const std::array<std::uint8_t, Size>& bytes) noexcept {
    std::uint8_t combined = 0U;
    for (const std::uint8_t byte : bytes) {
        combined = static_cast<std::uint8_t>(combined | byte);
    }
    return combined != 0U;
}

}  // namespace

ControllerLinkFirmwareRuntime::~ControllerLinkFirmwareRuntime() {
    stop();
}

bool ControllerLinkFirmwareRuntime::start(
    const ControllerLinkFirmwareRuntimeStartConfig& startup_config,
    const ControllerMbedTlsPskCredentials& credentials,
    const ControllerInstanceIdProvider& instance_identity) noexcept {
    if (peer_session_created_ || running_) {
        return false;
    }

    const auto& session_config = startup_config.session;
    const bool credentials_available =
        contains_nonzero(credentials.psk) &&
        contains_nonzero(credentials.identity);
    const bool instance_identity_generated = instance_identity.is_generated();
    const std::string_view generated_instance_id = instance_identity.value();
    const bool instance_configuration_matches =
        session_config.local_controller_instance_id.empty() ||
        (instance_identity_generated &&
         session_config.local_controller_instance_id == generated_instance_id);

    last_startup_readiness_ =
        hardware::interfaces::evaluate_controller_link_startup_readiness({
            startup_config.hardware_profile_confirmed,
            ControllerUartTransport::is_valid_config(startup_config.uart),
            credentials_available,
            instance_identity_generated,
            instance_configuration_matches,
            session_config.protocol_version.major,
            session_config.local_controller_id,
            session_config.peer_controller_id
        });
    if (!last_startup_readiness_.is_ready()) {
        // Fail closed before opening UART, initializing TLS or touching NVS.
        return false;
    }
    if (generated_instance_id.size() !=
        hardware::interfaces::kControllerInstanceIdTextSize) {
        return false;
    }

    // The generated provider is the single source of truth for this boot.
    // Readiness above rejects a conflicting configured ID before UART access.
    auto resolved_session_config = session_config;
    resolved_session_config.local_controller_instance_id = generated_instance_id;

    // Validate all session identifiers before touching the UART hardware.
    // ControllerLinkPeerSession copies them into bounded owned storage.
    hardware::interfaces::ControllerLinkPeerSession config_check{
        resolved_session_config};
    if (!config_check.is_configured()) {
        return false;
    }

    uart_config_ = startup_config.uart;
    if (!uart_.open(uart_config_)) {
        last_uart_error_ = uart_.last_error();
        state_ = ControllerLinkFirmwareRuntimeState::Failed;
        return false;
    }

    const bool peer_started = peer_service_.start(
        credentials,
        resolved_session_config,
        &uart_,
        &ControllerUartTransport::send_callback,
        &ControllerUartTransport::receive_callback);
    // ControllerLinkPeerService creates its peer-session object before TLS
    // initialization. If TLS setup fails, restart_transport() can retry while
    // retaining that same session and its sequence/replay history.
    peer_session_created_ = true;
    if (!peer_started) {
        last_uart_error_ = uart_.last_error();
        uart_.close();
        state_ = ControllerLinkFirmwareRuntimeState::Failed;
        return false;
    }

    last_uart_error_ = ESP_OK;
    reset_connection_liveness();
    running_ = true;
    state_ = ControllerLinkFirmwareRuntimeState::Running;
    return true;
}

bool ControllerLinkFirmwareRuntime::restart_transport(
    const ControllerMbedTlsPskCredentials& credentials) noexcept {
    if (!peer_session_created_ || running_) {
        return false;
    }

    const auto peer_state = peer_service_.status().state;
    if (peer_state != ControllerLinkPeerServiceState::Error &&
        peer_state != ControllerLinkPeerServiceState::Stopped) {
        return false;
    }

    if (!uart_.open(uart_config_)) {
        last_uart_error_ = uart_.last_error();
        state_ = ControllerLinkFirmwareRuntimeState::Failed;
        return false;
    }

    const bool restarted = peer_service_.restart_transport(
        credentials,
        &uart_,
        &ControllerUartTransport::send_callback,
        &ControllerUartTransport::receive_callback);
    if (!restarted) {
        last_uart_error_ = uart_.last_error();
        uart_.close();
        state_ = ControllerLinkFirmwareRuntimeState::Failed;
        return false;
    }

    last_uart_error_ = ESP_OK;
    reset_connection_liveness();
    running_ = true;
    state_ = ControllerLinkFirmwareRuntimeState::Running;
    return true;
}

void ControllerLinkFirmwareRuntime::poll(std::string_view sent_at) noexcept {
    if (!running_) {
        return;
    }

    if (!uart_.poll_transport_events()) {
        const esp_err_t transport_error = uart_.last_error();
        // An overrun or malformed UART frame invalidates the protected stream.
        // Destroy TLS before closing its injected UART callback context.
        peer_service_.stop();
        uart_.close();
        last_uart_error_ = transport_error != ESP_OK
            ? transport_error
            : uart_.last_error();
        running_ = false;
        state_ = ControllerLinkFirmwareRuntimeState::Failed;
        return;
    }

    peer_service_.poll(sent_at);
    const auto peer_status = peer_service_.status();
    if (peer_status.messages_received != last_peer_message_count_) {
        last_peer_message_count_ = peer_status.messages_received;
        last_peer_activity_at_us_ = monotonic_microseconds();
    }

    const std::uint64_t now_us = monotonic_microseconds();
    const bool handshake_pending =
        peer_status.state == ControllerLinkPeerServiceState::TlsHandshake ||
        peer_status.state == ControllerLinkPeerServiceState::AwaitingHello;
    const bool handshake_timed_out =
        handshake_pending &&
        elapsed_at_least(now_us, connection_started_at_us_,
                         kControllerHandshakeTimeoutUs);
    const bool heartbeat_timed_out =
        peer_status.state == ControllerLinkPeerServiceState::Established &&
        elapsed_at_least(now_us, last_peer_activity_at_us_,
                         kControllerHeartbeatTimeoutUs);

    if (handshake_timed_out || heartbeat_timed_out) {
        ESP_LOGW(
            kTag,
            handshake_timed_out
                ? "ControllerLink TLS/HELLO handshake timed out"
                : "ControllerLink peer heartbeat timed out");
        peer_service_.stop();
        const esp_err_t transport_error = uart_.last_error();
        uart_.close();
        last_uart_error_ = transport_error != ESP_OK
            ? transport_error
            : uart_.last_error();
        running_ = false;
        state_ = ControllerLinkFirmwareRuntimeState::Failed;
        return;
    }

    const auto peer_state = peer_status.state;
    if (peer_state == ControllerLinkPeerServiceState::Error ||
        peer_state == ControllerLinkPeerServiceState::Stopped) {
        // The peer service has already closed TLS and retained its diagnostic.
        // Do not call stop() again; close only the UART transport it used.
        const esp_err_t transport_error = uart_.last_error();
        uart_.close();
        last_uart_error_ = transport_error != ESP_OK
            ? transport_error
            : uart_.last_error();
        running_ = false;
        state_ = peer_state == ControllerLinkPeerServiceState::Error
            ? ControllerLinkFirmwareRuntimeState::Failed
            : ControllerLinkFirmwareRuntimeState::Stopped;
    }
}

void ControllerLinkFirmwareRuntime::reset_connection_liveness() noexcept {
    connection_started_at_us_ = monotonic_microseconds();
    last_peer_activity_at_us_ = connection_started_at_us_;
    last_peer_message_count_ = peer_service_.status().messages_received;
}

bool ControllerLinkFirmwareRuntime::snapshot_request_pending() const noexcept {
    return running_ && peer_service_.snapshot_request_pending();
}

bool ControllerLinkFirmwareRuntime::send_telemetry_snapshot(
    const hardware::interfaces::ControllerTelemetrySnapshotPayload& snapshot,
    std::string_view sent_at) noexcept {
    return running_ && peer_service_.send_telemetry_snapshot(snapshot, sent_at);
}

bool ControllerLinkFirmwareRuntime::export_authenticated_session_proof(
    std::array<std::uint8_t,
               hardware::interfaces::kControllerLinkSessionProofSize>& output) noexcept {
    if (!running_) {
        output.fill(0U);
        return false;
    }
    return peer_service_.export_authenticated_session_proof(output);
}

ControllerLinkFirmwareRuntimeStatus ControllerLinkFirmwareRuntime::status() const noexcept {
    return {
        state_,
        last_uart_error_,
        peer_service_.status(),
        last_startup_readiness_
    };
}

void ControllerLinkFirmwareRuntime::stop() noexcept {
    if (peer_session_created_) {
        // Close TLS/BIO first; only then remove the UART driver referenced by BIO.
        peer_service_.stop();
    }
    uart_.close();
    last_uart_error_ = uart_.last_error();
    connection_started_at_us_ = 0U;
    last_peer_activity_at_us_ = 0U;
    last_peer_message_count_ = 0U;
    running_ = false;
    // Do not report a clean stop while the UART driver is still owned after
    // uart_driver_delete() failed. A later stop()/restart may retry the close.
    state_ = uart_.is_open()
        ? ControllerLinkFirmwareRuntimeState::Failed
        : ControllerLinkFirmwareRuntimeState::Stopped;
}

}  // namespace dockstation::firmware::esp32
