#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <openssl/crypto.h>

#include "platform/linux/controller_psk_tls_client.hpp"
#include "platform/linux/controller_uart_port.hpp"

namespace dockstation::platform::linux_platform {

inline constexpr std::size_t kControllerTransportBufferSize = 4096U;
inline constexpr std::size_t kControllerTransportPumpLimit = 12U;

enum class ControllerTransportStatus : std::uint8_t {
    NotStarted,
    Started,
    Progress,
    WouldBlock,
    HandshakeComplete,
    InvalidArgument,
    Disconnected,
    PeerClosed,
    TlsFailed,
    TransportFailed
};

struct ControllerTransportResult final {
    ControllerTransportStatus status{ControllerTransportStatus::TransportFailed};
    std::size_t encrypted_bytes_received{0U};
    std::size_t encrypted_bytes_sent{0U};
    int system_error{0};
    unsigned long openssl_error{0UL};
};

// Coordinates exactly one Linux UART and one Repka-side TLS client.
// UART bytes are always treated as TLS ciphertext. Application data is exposed
// only through the TLS client after its complete PSK-DHE handshake succeeds.
//
// This class is single-thread/event-loop owned. The caller periodically invokes
// poll(), retains application bytes not accepted by write_application_data(),
// and consumes plaintext returned by read_application_data(). It does not own
// the credential store, ControllerCodec, link session, timers, or Gateway API.
class ControllerTransportManager final {
public:
    ControllerTransportManager() noexcept = default;
    ~ControllerTransportManager();

    ControllerTransportManager(const ControllerTransportManager&) = delete;
    ControllerTransportManager& operator=(const ControllerTransportManager&) = delete;
    ControllerTransportManager(ControllerTransportManager&&) = delete;
    ControllerTransportManager& operator=(ControllerTransportManager&&) = delete;

    // Starts a fresh UART + TLS client session. The caller obtains credentials
    // from ControllerPskCredentialStore and keeps that policy outside the link
    // pump; this method never reads, logs, or persists secrets.
    [[nodiscard]] ControllerTransportResult start(
        const char* absolute_device_path,
        const ControllerPskCredentials& credentials) noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_started() const noexcept { return active_; }
    [[nodiscard]] bool is_failed() const noexcept { return failed_; }
    [[nodiscard]] bool is_handshake_complete() const noexcept {
        return active_ && tls_.is_handshake_complete();
    }
    [[nodiscard]] bool export_session_proof(
        const std::uint8_t* context, std::size_t context_size,
        std::array<std::uint8_t, kControllerSessionProofSize>& output) const noexcept {
        OPENSSL_cleanse(output.data(), output.size());
        return is_handshake_complete() &&
               tls_.export_session_proof(context, context_size, output);
    }

    // Bounded non-blocking progress step. It preserves any partially transferred
    // ciphertext in fixed buffers. The caller should schedule this method again
    // after either progress or WouldBlock; heartbeat/deadline policy belongs above.
    [[nodiscard]] ControllerTransportResult poll() noexcept;

    // These methods return the TLS layer's exact status and byte count. If a
    // write/read is partial or WouldBlock, the caller must retry only the
    // unconsumed application bytes. Ciphertext produced/received is moved by poll().
    [[nodiscard]] ControllerTlsIoResult write_application_data(
        const std::uint8_t* bytes, std::size_t byte_count) noexcept;
    [[nodiscard]] ControllerTlsIoResult read_application_data(
        std::uint8_t* destination, std::size_t destination_capacity) noexcept;

    [[nodiscard]] int negotiated_tls_version() const noexcept {
        return is_handshake_complete() ? tls_.negotiated_tls_version() : 0;
    }
    [[nodiscard]] const char* negotiated_cipher_name() const noexcept {
        return is_handshake_complete() ? tls_.negotiated_cipher_name() : nullptr;
    }
    [[nodiscard]] int negotiated_group_id() const noexcept {
        return is_handshake_complete() ? tls_.negotiated_group_id() : 0;
    }
    [[nodiscard]] int last_system_error() const noexcept { return last_system_error_; }
    [[nodiscard]] unsigned long last_openssl_error() const noexcept {
        return last_openssl_error_;
    }

private:
    [[nodiscard]] ControllerTransportResult fail(
        ControllerTransportStatus status,
        int system_error = 0,
        unsigned long openssl_error = 0UL) noexcept;
    [[nodiscard]] ControllerTransportResult make_result(
        ControllerTransportStatus status,
        std::size_t received = 0U,
        std::size_t sent = 0U) const noexcept;
    [[nodiscard]] ControllerTransportResult map_uart_failure(
        const ControllerUartIoResult& result) noexcept;
    [[nodiscard]] ControllerTransportResult map_tls_failure() noexcept;
    void clear_pending_buffers() noexcept;

    ControllerUartPort uart_{};
    ControllerPskTlsClient tls_{};
    std::array<std::uint8_t, kControllerTransportBufferSize> inbound_ciphertext_{};
    std::array<std::uint8_t, kControllerTransportBufferSize> outbound_ciphertext_{};
    std::size_t inbound_offset_{0U};
    std::size_t inbound_size_{0U};
    std::size_t outbound_offset_{0U};
    std::size_t outbound_size_{0U};
    bool active_{false};
    bool failed_{false};
    ControllerTransportStatus failure_status_{ControllerTransportStatus::NotStarted};
    int last_system_error_{0};
    unsigned long last_openssl_error_{0UL};
};

}  // namespace dockstation::platform::linux_platform
