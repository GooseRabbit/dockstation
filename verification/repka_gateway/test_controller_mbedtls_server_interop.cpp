#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ssl.h>
#include <openssl/crypto.h>

#include "controller_mbedtls_psk_server.hpp"
#include "platform/linux/controller_psk_tls_client.hpp"

using namespace dockstation::firmware::esp32;
using namespace dockstation::platform::linux_platform;
using namespace dockstation::hardware::interfaces;

namespace {
int failures = 0;
int checks = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

struct SocketContext final { int fd{-1}; };

int socket_send(void* context, const unsigned char* bytes, std::size_t count) noexcept {
    if (context == nullptr || bytes == nullptr || count == 0U)
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    const int fd = static_cast<SocketContext*>(context)->fd;
    for (;;) {
        const auto sent = ::send(fd, bytes, count, MSG_NOSIGNAL);
        if (sent > 0) return static_cast<int>(sent);
        if (sent < 0 && errno == EINTR) continue;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

int socket_receive(void* context, unsigned char* bytes, std::size_t capacity) noexcept {
    if (context == nullptr || bytes == nullptr || capacity == 0U)
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    const int fd = static_cast<SocketContext*>(context)->fd;
    for (;;) {
        const auto received = ::recv(fd, bytes, capacity, 0);
        if (received >= 0) return static_cast<int>(received);
        if (errno == EINTR) continue;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

void fill_credentials(ControllerMbedTlsPskCredentials& server,
                      ControllerPskCredentials& client,
                      bool correct_identity = true,
                      bool correct_psk = true) {
    for (std::size_t i = 0U; i < server.psk.size(); ++i) {
        server.psk[i] = static_cast<std::uint8_t>(i + 1U);
        client.psk[i] = correct_psk
            ? server.psk[i] : static_cast<std::uint8_t>(0xA0U + i);
    }
    for (std::size_t i = 0U; i < server.identity.size(); ++i) {
        server.identity[i] = static_cast<std::uint8_t>(0x40U + i);
        client.identity[i] = correct_identity
            ? server.identity[i] : static_cast<std::uint8_t>(0x70U + i);
    }
}

bool flush_client(ControllerPskTlsClient& client, int fd) {
    std::array<std::uint8_t, 4096U> buffer{};
    for (std::size_t i = 0U; i < 32U; ++i) {
        const auto taken = client.take_encrypted_bytes(buffer.data(), buffer.size());
        if (taken.status == ControllerTlsIoStatus::WouldBlock) return true;
        if (taken.status != ControllerTlsIoStatus::Success) return false;
        if (taken.bytes_transferred == 0U) return true;
        std::size_t offset = 0U;
        while (offset < taken.bytes_transferred) {
            const auto written = ::send(fd, buffer.data() + offset,
                                        taken.bytes_transferred - offset, MSG_NOSIGNAL);
            if (written > 0) offset += static_cast<std::size_t>(written);
            else if (written < 0 && errno == EINTR) continue;
            else return false;
        }
    }
    return true;
}

bool receive_client(ControllerPskTlsClient& client, int fd, int timeout_ms) {
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1U, timeout_ms);
    if (ready == 0) return true;
    if (ready < 0) return errno == EINTR;
    if ((descriptor.revents & POLLIN) == 0 &&
        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;

    std::array<std::uint8_t, 4096U> buffer{};
    const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (count <= 0) return false;
    std::size_t offset = 0U;
    const auto size = static_cast<std::size_t>(count);
    while (offset < size) {
        const auto fed = client.feed_encrypted_bytes(buffer.data() + offset, size - offset);
        offset += fed.bytes_transferred;
        if (fed.status == ControllerTlsIoStatus::Failed ||
            fed.status == ControllerTlsIoStatus::NotConfigured ||
            fed.status == ControllerTlsIoStatus::InvalidArgument) return false;
        if (fed.bytes_transferred == 0U &&
            fed.status == ControllerTlsIoStatus::WouldBlock) return false;
    }
    return true;
}

bool drive_handshake(ControllerPskTlsClient& client, int fd) {
    for (std::size_t attempt = 0U; attempt < 400U; ++attempt) {
        if (client.is_handshake_complete()) return flush_client(client, fd);
        const auto progress = client.advance_handshake();
        if (progress == ControllerTlsProgress::Failed ||
            progress == ControllerTlsProgress::NotConfigured) return false;
        if (!flush_client(client, fd)) return false;
        if (client.is_handshake_complete()) return flush_client(client, fd);
        if (!receive_client(client, fd, 20)) return false;
    }
    return client.is_handshake_complete();
}

bool write_client_data(ControllerPskTlsClient& client, int fd,
                       const std::uint8_t* bytes, std::size_t count) {
    std::size_t offset = 0U;
    for (std::size_t attempt = 0U; offset < count && attempt < 100U; ++attempt) {
        const auto written = client.write_application_data(bytes + offset, count - offset);
        if (written.bytes_transferred > 0U) {
            offset += written.bytes_transferred;
            if (!flush_client(client, fd)) return false;
        }
        if (written.status == ControllerTlsIoStatus::Success) continue;
        if (written.status != ControllerTlsIoStatus::WouldBlock ||
            !receive_client(client, fd, 20)) return false;
    }
    return offset == count && flush_client(client, fd);
}

bool read_client_data(ControllerPskTlsClient& client, int fd,
                      std::array<std::uint8_t, 16U>& out, std::size_t& out_size) {
    for (std::size_t attempt = 0U; attempt < 200U; ++attempt) {
        const auto read = client.read_application_data(out.data(), out.size());
        if (read.status == ControllerTlsIoStatus::Success) {
            out_size = read.bytes_transferred;
            return out_size > 0U;
        }
        if (read.status != ControllerTlsIoStatus::WouldBlock ||
            !receive_client(client, fd, 20)) return false;
    }
    return false;
}

struct ServerResult final {
    std::atomic<bool> handshake{false};
    std::atomic<bool> proof_ready{false};
    std::atomic<bool> app_exchange{false};
    std::atomic<int> error{0};
    std::array<std::uint8_t, kControllerLinkSessionProofSize> proof{};
};

void run_server(int fd, const ControllerMbedTlsPskCredentials& credentials,
                ServerResult& result, bool exchange_data) {
    SocketContext transport{fd};
    ControllerMbedTlsPskServer server;
    if (!server.initialize(credentials, &transport, socket_send, socket_receive)) {
        result.error.store(server.last_mbedtls_error());
        (void)::shutdown(fd, SHUT_RDWR);
        return;
    }
    if (server.advance_handshake() != ControllerMbedTlsHandshakeProgress::Complete) {
        const int error = server.last_mbedtls_error();
        result.error.store(error == 0 ? MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE : error);
        (void)::shutdown(fd, SHUT_RDWR);
        return;
    }
    result.handshake.store(true, std::memory_order_release);
    if (!server.export_session_proof(result.proof)) {
        result.error.store(MBEDTLS_ERR_SSL_INTERNAL_ERROR);
        (void)::shutdown(fd, SHUT_RDWR);
        return;
    }
    result.proof_ready.store(true, std::memory_order_release);
    if (!exchange_data) return;

    std::array<std::uint8_t, 16U> request{};
    const auto received = server.read_application_data(request.data(), request.size());
    static constexpr std::uint8_t ping[] = {'P','I','N','G'};
    if (received.status != ControllerMbedTlsIoStatus::Success ||
        received.bytes_transferred != sizeof(ping) ||
        std::memcmp(request.data(), ping, sizeof(ping)) != 0) {
        result.error.store(server.last_mbedtls_error() == 0
                               ? MBEDTLS_ERR_SSL_INTERNAL_ERROR
                               : server.last_mbedtls_error());
        return;
    }

    static constexpr std::uint8_t pong[] = {'P','O','N','G'};
    const auto sent = server.write_application_data(pong, sizeof(pong));
    if (sent.status != ControllerMbedTlsIoStatus::Success ||
        sent.bytes_transferred != sizeof(pong)) {
        result.error.store(server.last_mbedtls_error() == 0
                               ? MBEDTLS_ERR_SSL_INTERNAL_ERROR
                               : server.last_mbedtls_error());
        return;
    }
    result.app_exchange.store(true, std::memory_order_release);
}

bool make_socket_pair(int sockets[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0;
}

bool successful_interop() {
    int sockets[2]{-1, -1};
    if (!make_socket_pair(sockets)) {
        expect(false, "creates a POSIX socket pair for TLS interop");
        return false;
    }

    ControllerMbedTlsPskCredentials server_credentials{};
    ControllerPskCredentials client_credentials{};
    fill_credentials(server_credentials, client_credentials);
    ServerResult server_result{};
    std::thread server_thread(run_server, sockets[1],
                              std::cref(server_credentials),
                              std::ref(server_result), true);

    ControllerPskTlsClient client;
    const bool initialized = client.initialize(client_credentials);
    const bool handshake = initialized && drive_handshake(client, sockets[0]);

    std::array<std::uint8_t, kControllerLinkSessionProofSize> client_proof{};
    const bool client_proof_ready = handshake && client.export_session_proof(
        reinterpret_cast<const std::uint8_t*>(kControllerSessionProofContext),
        sizeof(kControllerSessionProofContext) - 1U, client_proof);

    bool server_proof_ready = false;
    for (std::size_t i = 0U; i < 200U; ++i) {
        if (server_result.proof_ready.load(std::memory_order_acquire)) {
            server_proof_ready = true;
            break;
        }
        if (!handshake) break;
        (void)::poll(nullptr, 0U, 5);
    }
    expect(initialized && handshake && server_result.handshake.load(),
           "completes TLS 1.3 PSK-DHE handshake between OpenSSL and Mbed TLS");
    expect(client_proof_ready && server_proof_ready &&
               client_proof == server_result.proof,
           "matches the 32-byte exporter output and shared label/context");

    static constexpr std::uint8_t ping[] = {'P','I','N','G'};
    const bool ping_sent = handshake &&
        write_client_data(client, sockets[0], ping, sizeof(ping));
    std::array<std::uint8_t, 16U> response{};
    std::size_t response_size = 0U;
    const bool pong_received = ping_sent &&
        read_client_data(client, sockets[0], response, response_size);
    static constexpr std::uint8_t pong[] = {'P','O','N','G'};

    bool server_exchange = false;
    for (std::size_t i = 0U; i < 100U; ++i) {
        if (server_result.app_exchange.load(std::memory_order_acquire)) {
            server_exchange = true;
            break;
        }
        if (server_result.error.load(std::memory_order_acquire) != 0) break;
        (void)::poll(nullptr, 0U, 5);
    }
    expect(pong_received && response_size == sizeof(pong) &&
               std::memcmp(response.data(), pong, sizeof(pong)) == 0 &&
               server_exchange,
           "exchanges authenticated application data in both directions");

    client.close();
    (void)::shutdown(sockets[0], SHUT_RDWR);
    ::close(sockets[0]);
    server_thread.join();
    ::close(sockets[1]);
    server_credentials.clear();
    OPENSSL_cleanse(client_proof.data(), client_proof.size());
    OPENSSL_cleanse(server_result.proof.data(), server_result.proof.size());
    return initialized && handshake && client_proof_ready &&
           server_proof_ready && server_exchange && pong_received;
}

bool rejected_interop(bool use_correct_identity, bool use_correct_psk) {
    int sockets[2]{-1, -1};
    if (!make_socket_pair(sockets)) return false;

    ControllerMbedTlsPskCredentials server_credentials{};
    ControllerPskCredentials client_credentials{};
    fill_credentials(server_credentials, client_credentials,
                     use_correct_identity, use_correct_psk);
    ServerResult server_result{};
    std::thread server_thread(run_server, sockets[1],
                              std::cref(server_credentials),
                              std::ref(server_result), false);

    ControllerPskTlsClient client;
    const bool initialized = client.initialize(client_credentials);
    const bool handshake = initialized && drive_handshake(client, sockets[0]);
    const bool rejected = client.is_failed() && !handshake;

    (void)::shutdown(sockets[0], SHUT_RDWR);
    client.close();
    ::close(sockets[0]);
    server_thread.join();
    ::close(sockets[1]);
    server_credentials.clear();
    return initialized && rejected && !server_result.handshake.load();
}

}  // namespace

int main() {
    ControllerMbedTlsPskServer unconfigured{};
    std::array<std::uint8_t, kControllerLinkSessionProofSize> proof{};
    expect(unconfigured.advance_handshake() ==
               ControllerMbedTlsHandshakeProgress::NotConfigured,
           "does not start before pair credentials are configured");
    expect(!unconfigured.export_session_proof(proof),
           "does not export proof before TLS handshake completion");
    expect(successful_interop(),
           "passes OpenSSL client to Mbed TLS server TLS 1.3 PSK-DHE interop");
    expect(rejected_interop(false, true),
           "rejects incorrect 16-byte PSK identity");
    expect(rejected_interop(true, false),
           "rejects incorrect 32-byte PSK");
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
