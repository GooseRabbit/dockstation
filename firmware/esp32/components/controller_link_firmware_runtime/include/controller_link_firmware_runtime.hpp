#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "controller_instance_id_provider.hpp"
#include "controller_link_peer_service.hpp"
#include "hardware/interfaces/controller_link_startup_readiness.hpp"
#include "controller_mbedtls_psk_server.hpp"
#include "controller_uart_transport.hpp"

namespace dockstation::firmware::esp32 {

enum class ControllerLinkFirmwareRuntimeState : std::uint8_t {
    NotConfigured,
    Running,
    Failed,
    Stopped
};

struct ControllerLinkFirmwareRuntimeStartConfig final {
    // Must be true only after the exact module/SoC, Flash/partition layout,
    // UART pinout and electrical compatibility have been physically verified.
    bool hardware_profile_confirmed{false};
    ControllerUartConfig uart{};
    hardware::interfaces::ControllerLinkPeerSessionConfig session{};
};

struct ControllerLinkFirmwareRuntimeStatus final {
    ControllerLinkFirmwareRuntimeState state{
        ControllerLinkFirmwareRuntimeState::NotConfigured};
    esp_err_t last_uart_error{ESP_OK};
    ControllerLinkPeerServiceStatus peer{};
    hardware::interfaces::ControllerLinkStartupReadiness startup_readiness{};
    ControllerUartFault last_uart_fault{ControllerUartFault::None};
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

    // Requires the already-generated per-boot identity provider. The runtime
    // injects its value into the peer-session config; a caller-supplied ID must
    // be empty or match it exactly.
    [[nodiscard]] bool start(
        const ControllerLinkFirmwareRuntimeStartConfig& startup_config,
        const ControllerMbedTlsPskCredentials& credentials,
        const ControllerInstanceIdProvider& instance_identity) noexcept;

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
    ControllerUartFault last_uart_fault_{ControllerUartFault::None};
    std::uint64_t connection_started_at_us_{0U};
    std::uint64_t last_peer_activity_at_us_{0U};
    std::uint64_t last_peer_message_count_{0U};
    bool peer_session_created_{false};
    bool running_{false};
    hardware::interfaces::ControllerLinkStartupReadiness last_startup_readiness_{};
    void reset_connection_liveness() noexcept;
};

}  // namespace dockstation::firmware::esp32
