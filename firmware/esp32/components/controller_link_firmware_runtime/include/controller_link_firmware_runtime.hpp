#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "controller_link_peer_service.hpp"
#include "controller_mbedtls_psk_server.hpp"
#include "controller_uart_transport.hpp"

namespace dockstation::firmware::esp32 {

enum class ControllerLinkFirmwareRuntimeState : std::uint8_t {
    NotConfigured,
    Running,
    Failed,
    Stopped
};

struct ControllerLinkFirmwareRuntimeStatus final {
    ControllerLinkFirmwareRuntimeState state{
        ControllerLinkFirmwareRuntimeState::NotConfigured};
    esp_err_t last_uart_error{ESP_OK};
    ControllerLinkPeerServiceStatus peer{};
};

// Owns the UART and peer service in one place. The peer service is always
// stopped before the UART driver is deleted, so Mbed TLS never retains a
// transport context whose underlying driver has already been removed.
// Credentials and sensor readings remain caller-owned inputs.
class ControllerLinkFirmwareRuntime final {
public:
    ControllerLinkFirmwareRuntime() noexcept = default;
    ~ControllerLinkFirmwareRuntime();

    ControllerLinkFirmwareRuntime(const ControllerLinkFirmwareRuntime&) = delete;
    ControllerLinkFirmwareRuntime& operator=(const ControllerLinkFirmwareRuntime&) = delete;
    ControllerLinkFirmwareRuntime(ControllerLinkFirmwareRuntime&&) = delete;
    ControllerLinkFirmwareRuntime& operator=(ControllerLinkFirmwareRuntime&&) = delete;

    [[nodiscard]] bool start(
        const ControllerUartConfig& uart_config,
        const ControllerMbedTlsPskCredentials& credentials,
        const hardware::interfaces::ControllerLinkPeerSessionConfig& session_config) noexcept;

    [[nodiscard]] bool restart_transport(
        const ControllerMbedTlsPskCredentials& credentials) noexcept;

    void poll(std::string_view sent_at) noexcept;

    [[nodiscard]] bool snapshot_request_pending() const noexcept;
    [[nodiscard]] bool send_telemetry_snapshot(
        const hardware::interfaces::ControllerTelemetrySnapshotPayload& snapshot,
        std::string_view sent_at) noexcept;
    [[nodiscard]] bool export_authenticated_session_proof(
        std::array<std::uint8_t,
                   hardware::interfaces::kControllerLinkSessionProofSize>& output) noexcept;

    [[nodiscard]] ControllerLinkFirmwareRuntimeStatus status() const noexcept;
    void stop() noexcept;

private:
    ControllerUartTransport uart_{};
    ControllerLinkPeerService peer_service_{};
    ControllerUartConfig uart_config_{};
    ControllerLinkFirmwareRuntimeState state_{
        ControllerLinkFirmwareRuntimeState::NotConfigured};
    esp_err_t last_uart_error_{ESP_OK};
    bool peer_session_created_{false};
    bool running_{false};
};

}  // namespace dockstation::firmware::esp32
