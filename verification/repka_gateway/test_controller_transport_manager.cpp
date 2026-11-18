#define _XOPEN_SOURCE 600

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <sys/file.h>
#include <unistd.h>

#include "platform/linux/controller_transport_manager.hpp"

using namespace dockstation::platform::linux_platform;

namespace {
int checks = 0;
int failures = 0;

void expect(bool ok, const char* name) {
    ++checks;
    if (ok) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

struct Pty final {
    int master{-1};
    std::string slave{};

    ~Pty() {
        if (master >= 0) {
            (void)::close(master);
        }
    }

    bool open() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            return false;
        }
        std::array<char, 256U> path{};
        if (::ptsname_r(master, path.data(), path.size()) != 0) {
            return false;
        }
        slave = path.data();
        return true;
    }
};

struct PeerCredentials final {
    ControllerPskCredentials value{};
};

int find_psk(SSL* ssl, const unsigned char* identity,
             std::size_t identity_size, SSL_SESSION** session) noexcept {
    if (ssl == nullptr || identity == nullptr || session == nullptr) {
        return 0;
    }
    auto* peer = static_cast<PeerCredentials*>(SSL_get_app_data(ssl));
    if (peer == nullptr || identity_size != peer->value.identity.size() ||
        CRYPTO_memcmp(identity, peer->value.identity.data(), identity_size) != 0) {
        return 0;
    }

    constexpr unsigned char suite_id[] = {0x13U, 0x01U};
    const SSL_CIPHER* cipher = SSL_CIPHER_find(ssl, suite_id);
    SSL_SESSION* value = SSL_SESSION_new();
    if (cipher == nullptr || value == nullptr ||
        SSL_SESSION_set1_master_key(value, peer->value.psk.data(),
                                    peer->value.psk.size()) != 1 ||
        SSL_SESSION_set_cipher(value, cipher) != 1 ||
        SSL_SESSION_set_protocol_version(value, TLS1_3_VERSION) != 1 ||
        SSL_SESSION_set_max_early_data(value, 0U) != 1) {
        SSL_SESSION_free(value);
        return 0;
    }
    *session = value;
    return 1;
}

bool make_credentials(ControllerPskCredentials& value) noexcept {
    return RAND_bytes(value.psk.data(), static_cast<int>(value.psk.size())) == 1 &&
           RAND_bytes(value.identity.data(), static_cast<int>(value.identity.size())) == 1;
}

struct TlsPeer final {
    SSL_CTX* context{nullptr};
    SSL* ssl{nullptr};
    BIO* transport{nullptr};
    PeerCredentials credentials{};

    ~TlsPeer() {
        SSL_free(ssl);
        BIO_free(transport);
        SSL_CTX_free(context);
        credentials.value.clear();
    }

    bool initialize(const ControllerPskCredentials& pair) {
        credentials.value = pair;
        context = SSL_CTX_new(TLS_server_method());
        if (context == nullptr ||
            SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_ciphersuites(context, "TLS_AES_128_GCM_SHA256") != 1 ||
            SSL_CTX_set1_groups_list(context, "P-256") != 1 ||
            SSL_CTX_set_max_early_data(context, 0U) != 1 ||
            SSL_CTX_set_num_tickets(context, 0U) != 1) {
            return false;
        }
        SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
        SSL_CTX_clear_options(context, SSL_OP_ALLOW_NO_DHE_KEX);
        SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
        SSL_CTX_set_psk_find_session_callback(context, &find_psk);

        ssl = SSL_new(context);
        if (ssl == nullptr) {
            return false;
        }
        SSL_set_app_data(ssl, &credentials);
        BIO* ssl_bio = nullptr;
        if (BIO_new_bio_pair(&ssl_bio, 0U, &transport, 0U) != 1 ||
            ssl_bio == nullptr || transport == nullptr) {
            BIO_free(ssl_bio);
            return false;
        }
        SSL_set_bio(ssl, ssl_bio, ssl_bio);
        SSL_set_accept_state(ssl);
        return true;
    }
};

bool write_all(int fd, const std::uint8_t* bytes, std::size_t count) noexcept {
    std::size_t offset = 0U;
    while (offset < count) {
        const ssize_t n = ::write(fd, bytes + offset, count - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool uart_to_peer(int master, BIO* peer) noexcept {
    std::array<std::uint8_t, 1024U> bytes{};
    for (std::size_t i = 0U; i < 32U; ++i) {
        const ssize_t n = ::read(master, bytes.data(), bytes.size());
        if (n > 0) {
            std::size_t written = 0U;
            if (BIO_write_ex(peer, bytes.data(), static_cast<std::size_t>(n),
                             &written) != 1 ||
                written != static_cast<std::size_t>(n)) {
                return false;
            }
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        } else {
            return n == 0;
        }
    }
    return false;
}

bool peer_to_uart(BIO* peer, int master) noexcept {
    std::array<std::uint8_t, 1024U> bytes{};
    for (std::size_t i = 0U; i < 32U; ++i) {
        std::size_t n = 0U;
        if (BIO_read_ex(peer, bytes.data(), bytes.size(), &n) != 1) {
            return BIO_should_retry(peer) != 0;
        }
        if (n == 0U) {
            return true;
        }
        if (!write_all(master, bytes.data(), n)) {
            return false;
        }
    }
    return false;
}

bool handshake(ControllerTransportManager& client, TlsPeer& peer, int master) noexcept {
    bool server_ready = false;
    for (std::size_t i = 0U; i < 256U; ++i) {
        const auto progress = client.poll();
        if (progress.status == ControllerTransportStatus::TlsFailed ||
            progress.status == ControllerTransportStatus::Disconnected ||
            progress.status == ControllerTransportStatus::TransportFailed) {
            return false;
        }
        if (!uart_to_peer(master, peer.transport)) {
            return false;
        }
        if (!server_ready) {
            const int rc = SSL_accept(peer.ssl);
            if (rc == 1) {
                server_ready = true;
            } else {
                const int error = SSL_get_error(peer.ssl, rc);
                if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                    return false;
                }
            }
        }
        if (!peer_to_uart(peer.transport, master)) {
            return false;
        }
        if (server_ready && client.is_handshake_complete()) {
            return true;
        }
    }
    return false;
}

bool exchange_small(ControllerTransportManager& client, TlsPeer& peer,
                    int master) noexcept {
    constexpr std::array<std::uint8_t, 7U> request{{'D','S','-','P','I','N','G'}};
    constexpr std::array<std::uint8_t, 7U> reply{{'D','S','-','P','O','N','G'}};
    std::array<std::uint8_t, 32U> buffer{};
    const auto sent = client.write_application_data(request.data(), request.size());
    if (sent.status != ControllerTlsIoStatus::Success ||
        sent.bytes_transferred != request.size()) {
        return false;
    }

    bool got_request = false;
    for (std::size_t i = 0U; i < 128U && !got_request; ++i) {
        (void)client.poll();
        if (!uart_to_peer(master, peer.transport)) {
            return false;
        }
        std::size_t count = 0U;
        const int rc = SSL_read_ex(peer.ssl, buffer.data(), buffer.size(), &count);
        if (rc == 1) {
            got_request = count == request.size() &&
                CRYPTO_memcmp(buffer.data(), request.data(), request.size()) == 0;
        } else {
            const int error = SSL_get_error(peer.ssl, rc);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                return false;
            }
        }
    }
    if (!got_request) {
        return false;
    }

    std::size_t count = 0U;
    if (SSL_write_ex(peer.ssl, reply.data(), reply.size(), &count) != 1 ||
        count != reply.size() || !peer_to_uart(peer.transport, master)) {
        return false;
    }
    for (std::size_t i = 0U; i < 128U; ++i) {
        (void)client.poll();
        const auto received = client.read_application_data(buffer.data(), buffer.size());
        if (received.status == ControllerTlsIoStatus::Success) {
            return received.bytes_transferred == reply.size() &&
                CRYPTO_memcmp(buffer.data(), reply.data(), reply.size()) == 0;
        }
        if (received.status != ControllerTlsIoStatus::WouldBlock) {
            return false;
        }
    }
    return false;
}

bool exchange_large(ControllerTransportManager& client, TlsPeer& peer,
                    int master) noexcept {
    std::array<std::uint8_t, 65536U> payload{};
    for (std::size_t i = 0U; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 31U) & 0xFFU);
    }
    std::array<std::uint8_t, 4096U> buffer{};
    std::size_t sent = 0U;
    std::size_t received = 0U;
    for (std::size_t i = 0U; i < 512U; ++i) {
        if (sent < payload.size()) {
            const auto result = client.write_application_data(
                payload.data() + sent, payload.size() - sent);
            if (result.bytes_transferred > payload.size() - sent ||
                (result.status != ControllerTlsIoStatus::Success &&
                 result.status != ControllerTlsIoStatus::WouldBlock)) {
                return false;
            }
            sent += result.bytes_transferred;
        }
        const auto progress = client.poll();
        if (progress.status == ControllerTransportStatus::TlsFailed ||
            progress.status == ControllerTransportStatus::Disconnected ||
            progress.status == ControllerTransportStatus::TransportFailed ||
            !uart_to_peer(master, peer.transport)) {
            return false;
        }
        for (std::size_t j = 0U; j < 64U; ++j) {
            std::size_t count = 0U;
            const int rc = SSL_read_ex(peer.ssl, buffer.data(), buffer.size(), &count);
            if (rc == 1) {
                if (count == 0U || count > payload.size() - received ||
                    CRYPTO_memcmp(buffer.data(), payload.data() + received, count) != 0) {
                    return false;
                }
                received += count;
            } else {
                const int error = SSL_get_error(peer.ssl, rc);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                    break;
                }
                return false;
            }
        }
        if (sent == payload.size() && received == payload.size()) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    ControllerPskCredentials credentials{};
    expect(make_credentials(credentials), "generates fresh pair credentials for host peer test");

    ControllerTransportManager idle;
    expect(idle.poll().status == ControllerTransportStatus::NotStarted,
           "poll reports NotStarted before manager start");
    expect(idle.read_application_data(nullptr, 1U).status ==
               ControllerTlsIoStatus::NotConfigured,
           "plaintext read is unavailable before start");
    expect(idle.start("dev/ttyS0", credentials).status ==
               ControllerTransportStatus::InvalidArgument,
           "rejects relative UART paths");

    Pty pty;
    expect(pty.open(), "creates POSIX PTY for host-only link test");
    if (pty.master < 0 || pty.slave.empty()) {
        std::cout << "Checks completed: " << checks << ", failures: " << failures << '\n';
        return EXIT_FAILURE;
    }

    const int lock_fd = ::open(
        pty.slave.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    expect(lock_fd >= 0 && ::flock(lock_fd, LOCK_EX | LOCK_NB) == 0,
           "holds a competing lock to simulate another UART owner");
    ControllerTransportManager busy_manager;
    const auto busy = busy_manager.start(pty.slave.c_str(), credentials);
    expect(busy.status == ControllerTransportStatus::DeviceBusy &&
               busy_manager.is_failed() && !busy_manager.is_started() &&
               (busy.system_error == EWOULDBLOCK || busy.system_error == EAGAIN),
           "propagates UART ownership conflict without starting TLS");
    if (lock_fd >= 0) {
        (void)::flock(lock_fd, LOCK_UN);
        (void)::close(lock_fd);
    }

    Pty stalled_peer;
    expect(stalled_peer.open(), "creates PTY for stalled TLS peer deadline test");
    ControllerTransportManager timed_manager(std::chrono::milliseconds{1});
    expect(timed_manager.start(stalled_peer.slave.c_str(), credentials).status ==
               ControllerTransportStatus::Started,
           "starts TLS client with a short deterministic handshake deadline");
    (void)timed_manager.poll();
    ::usleep(5000U);
    const auto timed_out = timed_manager.poll();
    expect(timed_out.status == ControllerTransportStatus::TlsHandshakeTimeout &&
               !timed_manager.is_started() && timed_manager.is_failed(),
           "fails and releases UART/TLS when peer never completes TLS handshake");

    ControllerTransportManager manager;
    const auto started = manager.start(pty.slave.c_str(), credentials);
    expect(started.status == ControllerTransportStatus::Started &&
               manager.is_started() && !manager.is_handshake_complete(),
           "starts UART and TLS without claiming an authenticated session");
    const std::array<std::uint8_t, 4U> early{{'N','O','P','E'}};
    const auto early_write = manager.write_application_data(early.data(), early.size());
    expect(early_write.status == ControllerTlsIoStatus::HandshakeIncomplete &&
               early_write.bytes_transferred == 0U,
           "blocks application plaintext until TLS handshake completes");

    TlsPeer peer;
    expect(peer.initialize(credentials),
           "initializes a TLS 1.3 PSK server peer with the same pair credentials");
    expect(handshake(manager, peer, pty.master) && manager.is_handshake_complete() &&
               manager.negotiated_tls_version() == TLS1_3_VERSION &&
               manager.negotiated_cipher_name() != nullptr &&
               std::strcmp(manager.negotiated_cipher_name(),
                           "TLS_AES_128_GCM_SHA256") == 0,
           "completes TLS 1.3 PSK-DHE after moving ciphertext through PTY");
    expect(exchange_small(manager, peer, pty.master),
           "moves encrypted application data both ways through managed UART");
    expect(exchange_large(manager, peer, pty.master),
           "preserves 64-KiB application stream across record chunks and backpressure");

    manager.close();
    expect(!manager.is_started() && !manager.is_failed() &&
               manager.poll().status == ControllerTransportStatus::NotStarted,
           "close releases both layers and resets manager state");

    ControllerTransportManager no_device;
    expect(no_device.start("/dev/dockstation-manager-missing-uart", credentials).status ==
               ControllerTransportStatus::TransportFailed && no_device.is_failed(),
           "fails closed when UART device cannot be opened");

    Pty disconnected;
    expect(disconnected.open(), "creates second PTY for transport-loss test");
    ControllerTransportManager disconnected_manager;
    expect(disconnected_manager.start(disconnected.slave.c_str(), credentials).status ==
               ControllerTransportStatus::Started,
           "starts manager before transport-loss test");
    (void)::close(disconnected.master);
    disconnected.master = -1;
    const auto disconnected_result = disconnected_manager.poll();
    expect(disconnected_result.status == ControllerTransportStatus::Disconnected &&
               !disconnected_manager.is_started() && disconnected_manager.is_failed(),
           "closes TLS state when UART peer disappears during handshake");

    ControllerPskCredentials wrong = credentials;
    expect(RAND_bytes(wrong.psk.data(), static_cast<int>(wrong.psk.size())) == 1,
           "generates a different PSK for negative handshake test");
    TlsPeer wrong_peer;
    expect(wrong_peer.initialize(wrong),
           "initializes peer that does not possess Repka pair key");
    ControllerTransportManager rejects_wrong;
    expect(rejects_wrong.start(pty.slave.c_str(), credentials).status ==
               ControllerTransportStatus::Started,
           "starts new session for wrong-key rejection test");
    bool rejected = false;
    for (std::size_t i = 0U; i < 64U && !rejected; ++i) {
        const auto progress = rejects_wrong.poll();
        if (progress.status == ControllerTransportStatus::TlsFailed) {
            rejected = true;
            break;
        }
        if (progress.status == ControllerTransportStatus::Disconnected ||
            progress.status == ControllerTransportStatus::TransportFailed ||
            !uart_to_peer(pty.master, wrong_peer.transport)) {
            break;
        }
        const int rc = SSL_accept(wrong_peer.ssl);
        if (rc != 1) {
            const int error = SSL_get_error(wrong_peer.ssl, rc);
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                // A wrong PSK/identity is expected to be rejected by the peer.
            }
        }
        if (!peer_to_uart(wrong_peer.transport, pty.master)) {
            break;
        }
    }
    expect(rejected && !rejects_wrong.is_started() && rejects_wrong.is_failed(),
           "rejects wrong PSK and tears down UART/TLS state");

    credentials.clear();
    wrong.clear();
    std::cout << "Checks completed: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
