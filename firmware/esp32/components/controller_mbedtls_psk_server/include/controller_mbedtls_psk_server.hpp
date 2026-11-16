#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include "hardware/interfaces/controller_link_crypto_profile.hpp"
namespace dockstation::firmware::esp32 {
enum class ControllerMbedTlsPskServerState : std::uint8_t {
    NotConfigured, AwaitingHandshake, Established, Failed, Closed
};
enum class ControllerMbedTlsHandshakeProgress : std::uint8_t {
    Complete, NeedRead, NeedWrite, NotConfigured, Failed
};
enum class ControllerMbedTlsIoStatus : std::uint8_t {
    Success, WouldBlock, HandshakeIncomplete, NotConfigured,
    Closed, InvalidArgument, Failed
};
struct ControllerMbedTlsIoResult final {
    ControllerMbedTlsIoStatus status{ControllerMbedTlsIoStatus::Failed};
    std::size_t bytes_transferred{0U};
};
struct ControllerMbedTlsPskCredentials final {
    std::array<std::uint8_t, hardware::interfaces::kControllerLinkPskSize> psk{};
    std::array<std::uint8_t, hardware::interfaces::kControllerLinkPskIdentitySize> identity{};
    ControllerMbedTlsPskCredentials() noexcept = default;
    ControllerMbedTlsPskCredentials(const ControllerMbedTlsPskCredentials&) = delete;
    ControllerMbedTlsPskCredentials& operator=(const ControllerMbedTlsPskCredentials&) = delete;
    ControllerMbedTlsPskCredentials(ControllerMbedTlsPskCredentials&&) = delete;
    ControllerMbedTlsPskCredentials& operator=(ControllerMbedTlsPskCredentials&&) = delete;
    void clear() noexcept;
    ~ControllerMbedTlsPskCredentials();
};

// TLS 1.3 PSK-DHE server for the ESP32 end of the internal Repka link.
// Byte-stream send/receive callbacks are injected by the caller; firmware
// binds them to UART, while host interop binds them to a POSIX socket.
class ControllerMbedTlsPskServer final {
public:
    ControllerMbedTlsPskServer() noexcept = default;
    ~ControllerMbedTlsPskServer();
    ControllerMbedTlsPskServer(const ControllerMbedTlsPskServer&) = delete;
    ControllerMbedTlsPskServer& operator=(const ControllerMbedTlsPskServer&) = delete;
    ControllerMbedTlsPskServer(ControllerMbedTlsPskServer&&) = delete;
    ControllerMbedTlsPskServer& operator=(ControllerMbedTlsPskServer&&) = delete;

    [[nodiscard]] bool initialize(
        const ControllerMbedTlsPskCredentials& credentials,
        void* transport_context,
        mbedtls_ssl_send_t* send_callback,
        mbedtls_ssl_recv_t* receive_callback) noexcept;
    [[nodiscard]] ControllerMbedTlsHandshakeProgress advance_handshake() noexcept;
    [[nodiscard]] ControllerMbedTlsPskServerState state() const noexcept { return state_; }
    [[nodiscard]] bool is_handshake_complete() const noexcept {
        return state_ == ControllerMbedTlsPskServerState::Established;
    }
    [[nodiscard]] int last_mbedtls_error() const noexcept { return last_mbedtls_error_; }

    [[nodiscard]] ControllerMbedTlsIoResult write_application_data(
        const std::uint8_t* bytes, std::size_t byte_count) noexcept;
    [[nodiscard]] ControllerMbedTlsIoResult read_application_data(
        std::uint8_t* destination, std::size_t destination_capacity) noexcept;
    // The caller must also gate use of exporter proof on HELLO/HELLO_ACK.
    [[nodiscard]] bool export_session_proof(
        std::array<std::uint8_t, hardware::interfaces::kControllerLinkSessionProofSize>& output) noexcept;
    void close() noexcept;

private:
    static int select_pair_psk(
        void* context, mbedtls_ssl_context* ssl,
        const unsigned char* identity, std::size_t identity_size) noexcept;
    void configure_tls() noexcept;
    [[nodiscard]] bool verify_negotiated_profile() const noexcept;
    void mark_failed(int error) noexcept;
    void release_resources() noexcept;

    mbedtls_entropy_context entropy_{};
    mbedtls_ctr_drbg_context ctr_drbg_{};
    mbedtls_ssl_config config_{};
    mbedtls_ssl_context ssl_{};
    std::array<std::uint8_t, hardware::interfaces::kControllerLinkPskSize> psk_{};
    std::array<std::uint8_t, hardware::interfaces::kControllerLinkPskIdentitySize> identity_{};
    ControllerMbedTlsPskServerState state_{ControllerMbedTlsPskServerState::NotConfigured};
    int last_mbedtls_error_{0};
    bool entropy_initialized_{false};
    bool ctr_drbg_initialized_{false};
    bool config_initialized_{false};
    bool ssl_initialized_{false};
    bool credentials_ready_{false};
    bool pair_identity_accepted_{false};
};
}  // namespace dockstation::firmware::esp32
