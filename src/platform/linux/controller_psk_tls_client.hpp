#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <openssl/ssl.h>

namespace dockstation::platform::linux_platform {

inline constexpr std::size_t kControllerPskSize = 32U;
inline constexpr std::size_t kControllerPskIdentitySize = 16U;

// Credentials are provisioned outside the application configuration and are
// supplied by the caller. initialize() copies them into this object; the caller
// remains responsible for securely clearing its own input copy. Neither value
// is logged or serialized by this class.
struct ControllerPskCredentials final {
    std::array<std::uint8_t, kControllerPskSize> psk{};
    std::array<std::uint8_t, kControllerPskIdentitySize> identity{};
};

enum class ControllerTlsProgress : std::uint8_t {
    Complete,
    NeedRead,
    NeedWrite,
    NotConfigured,
    Failed
};

enum class ControllerTlsIoStatus : std::uint8_t {
    Success,
    WouldBlock,
    HandshakeIncomplete,
    NotConfigured,
    Closed,
    InvalidArgument,
    Failed
};

struct ControllerTlsIoResult final {
    ControllerTlsIoStatus status{ControllerTlsIoStatus::Failed};
    std::size_t bytes_transferred{0U};
};

// TLS 1.3 external-PSK client for the Repka side of the internal controller
// link. The transport boundary is deliberately byte-oriented: callers feed
// raw encrypted bytes from their physical transport and drain encrypted bytes
// for transmission. This class does not open or emulate a UART device.
//
// Application plaintext is unavailable until the complete PSK-DHE handshake
// succeeds and the negotiated TLS version/cipher suite pass the fixed profile.
class ControllerPskTlsClient final {
public:
    ControllerPskTlsClient() noexcept = default;
    ~ControllerPskTlsClient();

    ControllerPskTlsClient(const ControllerPskTlsClient&) = delete;
    ControllerPskTlsClient& operator=(const ControllerPskTlsClient&) = delete;
    ControllerPskTlsClient(ControllerPskTlsClient&&) = delete;
    ControllerPskTlsClient& operator=(ControllerPskTlsClient&&) = delete;

    [[nodiscard]] bool initialize(const ControllerPskCredentials& credentials) noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_configured() const noexcept;
    [[nodiscard]] bool is_handshake_complete() const noexcept;
    [[nodiscard]] bool is_failed() const noexcept { return failed_; }

    // Advance the non-blocking TLS handshake. NeedRead/NeedWrite tells the
    // transport loop which direction must make progress before retrying.
    [[nodiscard]] ControllerTlsProgress advance_handshake() noexcept;

    // Input from the physical transport: encrypted bytes received from peer.
    [[nodiscard]] ControllerTlsIoResult feed_encrypted_bytes(
        const std::uint8_t* bytes, std::size_t byte_count) noexcept;

    // Output to the physical transport: encrypted TLS records to transmit.
    [[nodiscard]] ControllerTlsIoResult take_encrypted_bytes(
        std::uint8_t* destination, std::size_t destination_capacity) noexcept;

    // These methods are gated until TLS authentication has completed.
    [[nodiscard]] ControllerTlsIoResult write_application_data(
        const std::uint8_t* bytes, std::size_t byte_count) noexcept;
    [[nodiscard]] ControllerTlsIoResult read_application_data(
        std::uint8_t* destination, std::size_t destination_capacity) noexcept;

    [[nodiscard]] int negotiated_tls_version() const noexcept;
    [[nodiscard]] const char* negotiated_cipher_name() const noexcept;
    [[nodiscard]] int negotiated_group_id() const noexcept;
    [[nodiscard]] unsigned long last_openssl_error() const noexcept {
        return last_openssl_error_;
    }

private:
    static int use_psk_session(
        SSL* ssl,
        const EVP_MD* digest,
        const unsigned char** identity,
        std::size_t* identity_length,
        SSL_SESSION** session) noexcept;

    [[nodiscard]] bool configure_context() noexcept;
    [[nodiscard]] bool verify_negotiated_profile() noexcept;
    void mark_failed() noexcept;

    SSL_CTX* context_{nullptr};
    SSL* ssl_{nullptr};
    BIO* transport_bio_{nullptr};
    ControllerPskCredentials credentials_{};
    bool credentials_ready_{false};
    bool failed_{false};
    unsigned long last_openssl_error_{0UL};
};

}  // namespace dockstation::platform::linux_platform
