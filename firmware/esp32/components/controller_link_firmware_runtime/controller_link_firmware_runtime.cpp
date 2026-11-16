#include "controller_link_firmware_runtime.hpp"

namespace dockstation::firmware::esp32 {

ControllerLinkFirmwareRuntime::~ControllerLinkFirmwareRuntime() {
    stop();
}

bool ControllerLinkFirmwareRuntime::start(
    const ControllerUartConfig& uart_config,
    const ControllerMbedTlsPskCredentials& credentials,
    const ControllerInstanceIdProvider& instance_identity,
    const hardware::interfaces::ControllerLinkPeerSessionConfig& session_config) noexcept {
    if (peer_session_created_ || running_ || !instance_identity.is_generated()) {
        return false;
    }

    const std::string_view generated_instance_id = instance_identity.value();
    if (generated_instance_id.size() !=
        hardware::interfaces::kControllerInstanceIdTextSize) {
        return false;
    }

    // The generated provider is the single source of truth for this boot. An
    // explicitly supplied different ID is a stale/static configuration error.
    if (!session_config.local_controller_instance_id.empty() &&
        session_config.local_controller_instance_id != generated_instance_id) {
        return false;
    }

    auto resolved_session_config = session_config;
    resolved_session_config.local_controller_instance_id = generated_instance_id;

    // Validate all session identifiers before touching the UART hardware.
    // ControllerLinkPeerSession copies them into bounded owned storage.
    hardware::interfaces::ControllerLinkPeerSession config_check{
        resolved_session_config};
    if (!config_check.is_configured()) {
        return false;
    }

    uart_config_ = uart_config;
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
    const auto peer_state = peer_service_.status().state;
    if (peer_state == ControllerLinkPeerServiceState::Error) {
        const esp_err_t transport_error = uart_.last_error();
        // Error means the peer service has already closed TLS and retained
        // its Mbed TLS diagnostic. Do not call stop() again and erase it.
        uart_.close();
        last_uart_error_ = transport_error != ESP_OK
            ? transport_error
            : uart_.last_error();
        running_ = false;
        state_ = ControllerLinkFirmwareRuntimeState::Failed;
    } else if (peer_state == ControllerLinkPeerServiceState::Stopped) {
        const esp_err_t transport_error = uart_.last_error();
        // Stopped is also reached only after the peer service has closed TLS.
        uart_.close();
        last_uart_error_ = transport_error != ESP_OK
            ? transport_error
            : uart_.last_error();
        running_ = false;
        state_ = ControllerLinkFirmwareRuntimeState::Stopped;
    }
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
        peer_service_.status()
    };
}

void ControllerLinkFirmwareRuntime::stop() noexcept {
    if (peer_session_created_) {
        // Close TLS/BIO first; only then remove the UART driver referenced by BIO.
        peer_service_.stop();
    }
    uart_.close();
    last_uart_error_ = uart_.last_error();
    running_ = false;
    state_ = ControllerLinkFirmwareRuntimeState::Stopped;
}

}  // namespace dockstation::firmware::esp32
