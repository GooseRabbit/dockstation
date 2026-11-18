#include "platform/linux/controller_transport_manager.hpp"

#include <openssl/crypto.h>

#include <chrono>

namespace dockstation::platform::linux_platform {
namespace {

[[nodiscard]] ControllerTransportStatus map_uart_status(
    ControllerUartStatus status) noexcept {
    switch (status) {
        case ControllerUartStatus::Disconnected:
        case ControllerUartStatus::NotOpen:
            return ControllerTransportStatus::Disconnected;
        case ControllerUartStatus::InvalidArgument:
            return ControllerTransportStatus::InvalidArgument;
        case ControllerUartStatus::Busy:
            return ControllerTransportStatus::DeviceBusy;
        case ControllerUartStatus::Success:
        case ControllerUartStatus::WouldBlock:
            return ControllerTransportStatus::Progress;
        case ControllerUartStatus::InvalidDevice:
        case ControllerUartStatus::Failed:
            return ControllerTransportStatus::TransportFailed;
    }
    return ControllerTransportStatus::TransportFailed;
}

}  // namespace

ControllerTransportManager::ControllerTransportManager(
    std::chrono::milliseconds handshake_timeout) noexcept
    : handshake_timeout_(handshake_timeout.count() > 0
          ? handshake_timeout : kControllerTlsHandshakeTimeout) {}

ControllerTransportManager::~ControllerTransportManager() {
    close();
}

ControllerTransportResult ControllerTransportManager::start(
    const char* absolute_device_path,
    const ControllerPskCredentials& credentials) noexcept {
    close();

    const ControllerUartIoResult uart_result = uart_.open(absolute_device_path);
    if (uart_result.status != ControllerUartStatus::Success) {
        return fail(map_uart_status(uart_result.status), uart_result.system_error);
    }

    if (!tls_.initialize(credentials)) {
        return fail(ControllerTransportStatus::TlsFailed, 0,
                    tls_.last_openssl_error());
    }

    handshake_started_at_ = std::chrono::steady_clock::now();
    active_ = true;
    return make_result(ControllerTransportStatus::Started);
}

void ControllerTransportManager::close() noexcept {
    tls_.close();
    uart_.close();
    clear_pending_buffers();
    active_ = false;
    failed_ = false;
    failure_status_ = ControllerTransportStatus::NotStarted;
    last_system_error_ = 0;
    last_openssl_error_ = 0UL;
}

ControllerTransportResult ControllerTransportManager::poll() noexcept {
    if (!active_) {
        return make_result(failed_ ? failure_status_
                                   : ControllerTransportStatus::NotStarted);
    }

    // A silent or stalled peer must not leave the Repka link in TLS_HANDSHAKE
    // indefinitely. Use a monotonic deadline so wall-clock adjustments cannot
    // extend or prematurely expire the initial handshake.
    if (!tls_.is_handshake_complete() &&
        std::chrono::steady_clock::now() - handshake_started_at_ >= handshake_timeout_) {
        return fail(ControllerTransportStatus::TlsHandshakeTimeout);
    }

    const bool handshake_was_complete = tls_.is_handshake_complete();
    bool any_progress = false;
    std::size_t encrypted_received = 0U;
    std::size_t encrypted_sent = 0U;

    // Bound work per call so a busy UART cannot monopolize the caller's event
    // loop. Offsets are retained across WouldBlock and partial device I/O.
    for (std::size_t pass = 0U; pass < kControllerTransportPumpLimit; ++pass) {
        bool pass_progress = false;

        if (outbound_offset_ < outbound_size_) {
            const std::size_t remaining = outbound_size_ - outbound_offset_;
            const ControllerUartIoResult write_result = uart_.write_bytes(
                outbound_ciphertext_.data() + outbound_offset_, remaining);
            if (write_result.status == ControllerUartStatus::Success) {
                if (write_result.bytes_transferred > remaining) {
                    return fail(ControllerTransportStatus::TransportFailed);
                }
                outbound_offset_ += write_result.bytes_transferred;
                encrypted_sent += write_result.bytes_transferred;
                pass_progress = write_result.bytes_transferred > 0U;
                if (outbound_offset_ == outbound_size_) {
                    outbound_offset_ = 0U;
                    outbound_size_ = 0U;
                }
            } else if (write_result.status != ControllerUartStatus::WouldBlock) {
                return map_uart_failure(write_result);
            }
        }

        if (inbound_offset_ < inbound_size_) {
            const std::size_t remaining = inbound_size_ - inbound_offset_;
            const ControllerTlsIoResult feed_result = tls_.feed_encrypted_bytes(
                inbound_ciphertext_.data() + inbound_offset_, remaining);
            if (feed_result.bytes_transferred > remaining) {
                return fail(ControllerTransportStatus::TlsFailed, 0,
                            tls_.last_openssl_error());
            }
            inbound_offset_ += feed_result.bytes_transferred;
            pass_progress = pass_progress || feed_result.bytes_transferred > 0U;
            if (inbound_offset_ == inbound_size_) {
                inbound_offset_ = 0U;
                inbound_size_ = 0U;
            }
            if (feed_result.status == ControllerTlsIoStatus::Closed) {
                return fail(ControllerTransportStatus::PeerClosed);
            }
            if (feed_result.status == ControllerTlsIoStatus::Failed ||
                feed_result.status == ControllerTlsIoStatus::NotConfigured ||
                feed_result.status == ControllerTlsIoStatus::InvalidArgument) {
                return map_tls_failure();
            }
        }

        // Do not read a later UART chunk until all bytes of the previous chunk
        // have entered the TLS BIO; this retains byte-stream order.
        if (inbound_size_ == 0U) {
            const ControllerUartIoResult read_result = uart_.read_bytes(
                inbound_ciphertext_.data(), inbound_ciphertext_.size());
            if (read_result.status == ControllerUartStatus::Success) {
                if (read_result.bytes_transferred > inbound_ciphertext_.size()) {
                    return fail(ControllerTransportStatus::TransportFailed);
                }
                if (read_result.bytes_transferred > 0U) {
                    inbound_offset_ = 0U;
                    inbound_size_ = read_result.bytes_transferred;
                    encrypted_received += read_result.bytes_transferred;
                    pass_progress = true;
                }
            } else if (read_result.status != ControllerUartStatus::WouldBlock) {
                return map_uart_failure(read_result);
            }
        }

        // OpenSSL must be allowed to consume bytes already fed into its BIO,
        // even if the tail of this UART chunk is still pending. We do not read
        // a later chunk until that tail is fed, so ordering remains intact.
        if (!tls_.is_handshake_complete()) {
            const ControllerTlsProgress progress = tls_.advance_handshake();
            if (progress == ControllerTlsProgress::Failed) {
                return map_tls_failure();
            }
            if (progress == ControllerTlsProgress::Complete) {
                pass_progress = true;
            }
        }

        if (outbound_size_ == 0U) {
            const ControllerTlsIoResult take_result = tls_.take_encrypted_bytes(
                outbound_ciphertext_.data(), outbound_ciphertext_.size());
            if (take_result.bytes_transferred > outbound_ciphertext_.size()) {
                return fail(ControllerTransportStatus::TlsFailed, 0,
                            tls_.last_openssl_error());
            }
            if (take_result.status == ControllerTlsIoStatus::Success) {
                if (take_result.bytes_transferred > 0U) {
                    outbound_offset_ = 0U;
                    outbound_size_ = take_result.bytes_transferred;
                    pass_progress = true;
                }
            } else if (take_result.status == ControllerTlsIoStatus::Closed) {
                return fail(ControllerTransportStatus::PeerClosed);
            } else if (take_result.status != ControllerTlsIoStatus::WouldBlock) {
                return map_tls_failure();
            }
        }

        any_progress = any_progress || pass_progress;
        if (!pass_progress) {
            break;
        }
    }

    if (!handshake_was_complete && tls_.is_handshake_complete()) {
        return make_result(ControllerTransportStatus::HandshakeComplete,
                           encrypted_received, encrypted_sent);
    }
    return make_result(any_progress ? ControllerTransportStatus::Progress
                                    : ControllerTransportStatus::WouldBlock,
                       encrypted_received, encrypted_sent);
}

ControllerTlsIoResult ControllerTransportManager::write_application_data(
    const std::uint8_t* bytes, std::size_t byte_count) noexcept {
    if (!active_) {
        return {failed_ ? ControllerTlsIoStatus::Failed
                        : ControllerTlsIoStatus::NotConfigured, 0U};
    }

    const ControllerTlsIoResult result = tls_.write_application_data(bytes, byte_count);
    if (result.status == ControllerTlsIoStatus::Failed) {
        (void)map_tls_failure();
    } else if (result.status == ControllerTlsIoStatus::Closed) {
        (void)fail(ControllerTransportStatus::PeerClosed, 0,
                   tls_.last_openssl_error());
    }
    return result;
}

ControllerTlsIoResult ControllerTransportManager::read_application_data(
    std::uint8_t* destination, std::size_t destination_capacity) noexcept {
    if (!active_) {
        return {failed_ ? ControllerTlsIoStatus::Failed
                        : ControllerTlsIoStatus::NotConfigured, 0U};
    }

    const ControllerTlsIoResult result = tls_.read_application_data(
        destination, destination_capacity);
    if (result.status == ControllerTlsIoStatus::Failed) {
        (void)map_tls_failure();
    } else if (result.status == ControllerTlsIoStatus::Closed) {
        (void)fail(ControllerTransportStatus::PeerClosed, 0,
                   tls_.last_openssl_error());
    }
    return result;
}

ControllerTransportResult ControllerTransportManager::fail(
    ControllerTransportStatus status,
    int system_error,
    unsigned long openssl_error) noexcept {
    last_system_error_ = system_error;
    last_openssl_error_ = openssl_error;
    failure_status_ = status;
    failed_ = true;
    active_ = false;
    tls_.close();
    uart_.close();
    clear_pending_buffers();
    return make_result(status);
}

ControllerTransportResult ControllerTransportManager::make_result(
    ControllerTransportStatus status,
    std::size_t received,
    std::size_t sent) const noexcept {
    return {status, received, sent, last_system_error_, last_openssl_error_};
}

ControllerTransportResult ControllerTransportManager::map_uart_failure(
    const ControllerUartIoResult& result) noexcept {
    return fail(map_uart_status(result.status), result.system_error);
}

ControllerTransportResult ControllerTransportManager::map_tls_failure() noexcept {
    return fail(ControllerTransportStatus::TlsFailed, 0,
                tls_.last_openssl_error());
}

void ControllerTransportManager::clear_pending_buffers() noexcept {
    OPENSSL_cleanse(inbound_ciphertext_.data(), inbound_ciphertext_.size());
    OPENSSL_cleanse(outbound_ciphertext_.data(), outbound_ciphertext_.size());
    inbound_offset_ = 0U;
    inbound_size_ = 0U;
    outbound_offset_ = 0U;
    outbound_size_ = 0U;
}

}  // namespace dockstation::platform::linux_platform
