#define _XOPEN_SOURCE 600

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include "platform/linux/controller_uart_port.hpp"

using namespace dockstation::platform::linux_platform;

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

struct PtyPair final {
    int master{-1};
    std::string slave_name{};

    ~PtyPair() {
        if (master >= 0) {
            (void)::close(master);
        }
    }

    bool initialize() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            return false;
        }
        std::array<char, 256U> path{};
        if (::ptsname_r(master, path.data(), path.size()) != 0) {
            return false;
        }
        slave_name = path.data();
        return true;
    }
};

bool write_all(int descriptor, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0U;
    while (offset < size) {
        const ssize_t count = ::write(descriptor, data + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

int main() {
    ControllerUartPort unopened;
    std::array<std::uint8_t, 8U> buffer{};
    expect(unopened.read_bytes(buffer.data(), buffer.size()).status ==
               ControllerUartStatus::NotOpen,
           "read fails safely when port is not open");
    expect(unopened.write_bytes(buffer.data(), buffer.size()).status ==
               ControllerUartStatus::NotOpen,
           "write fails safely when port is not open");
    expect(unopened.open("dev/ttyS0").status == ControllerUartStatus::InvalidArgument,
           "rejects relative device paths");
    expect(unopened.open(nullptr).status == ControllerUartStatus::InvalidArgument,
           "rejects null device path");
    const auto missing_device = unopened.open("/dev/dockstation-uart-port-does-not-exist");
    expect(missing_device.status == ControllerUartStatus::Failed &&
               missing_device.system_error == ENOENT,
           "returns the underlying error when the absolute device path is missing");

    PtyPair pty;
    expect(pty.initialize(), "creates a POSIX pseudo-terminal for host-only transport testing");
    if (pty.master < 0 || pty.slave_name.empty()) {
        std::cout << "Checks completed: " << checks << ", failures: " << failures << '\n';
        return EXIT_FAILURE;
    }

    ControllerUartPort port;
    const auto opened = port.open(pty.slave_name.c_str());
    expect(opened.status == ControllerUartStatus::Success && port.is_open(),
           "opens a character device and configures raw UART mode");

    const auto empty_read = port.read_bytes(buffer.data(), buffer.size());
    expect(empty_read.status == ControllerUartStatus::WouldBlock &&
               empty_read.bytes_transferred == 0U,
           "empty non-blocking read reports WouldBlock");

    struct termios configured {};
    const int inspect_fd = ::open(pty.slave_name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    const bool inspected = inspect_fd >= 0 && ::tcgetattr(inspect_fd, &configured) == 0;
    if (inspect_fd >= 0) {
        (void)::close(inspect_fd);
    }
    expect(inspected && (configured.c_cflag & CSIZE) == CS8 &&
               (configured.c_cflag & PARENB) == 0 &&
               (configured.c_cflag & CSTOPB) == 0 &&
               cfgetispeed(&configured) == B115200 &&
               cfgetospeed(&configured) == B115200 &&
               (configured.c_iflag & (IXON | IXOFF | IXANY)) == 0 &&
               (configured.c_cflag & CRTSCTS) == 0 &&
               (configured.c_lflag & (ICANON | ECHO | ISIG)) == 0,
           "sets 115200 baud, 8 data bits, no parity, one stop bit, no software flow control");

    constexpr std::array<std::uint8_t, 6U> inbound{{0x17U, 0x03U, 0x03U, 0x00U, 0x01U, 0xA5U}};
    expect(write_all(pty.master, inbound.data(), inbound.size()),
           "peer writes opaque bytes to the pseudo-terminal");
    const auto read_result = port.read_bytes(buffer.data(), buffer.size());
    expect(read_result.status == ControllerUartStatus::Success &&
               read_result.bytes_transferred == inbound.size() &&
               std::memcmp(buffer.data(), inbound.data(), inbound.size()) == 0,
           "reads the exact byte stream without interpreting its contents");

    constexpr std::array<std::uint8_t, 5U> outbound{{0x16U, 0x03U, 0x03U, 0x00U, 0x00U}};
    const auto write_result = port.write_bytes(outbound.data(), outbound.size());
    std::array<std::uint8_t, outbound.size()> peer_buffer{};
    const ssize_t peer_count = ::read(pty.master, peer_buffer.data(), peer_buffer.size());
    expect(write_result.status == ControllerUartStatus::Success &&
               write_result.bytes_transferred == outbound.size() &&
               peer_count == static_cast<ssize_t>(outbound.size()) &&
               std::memcmp(peer_buffer.data(), outbound.data(), outbound.size()) == 0,
           "writes opaque bytes to the peer without adding framing");

    expect(port.read_bytes(nullptr, 1U).status == ControllerUartStatus::InvalidArgument,
           "rejects a null read buffer for a non-empty request");
    expect(port.write_bytes(nullptr, 1U).status == ControllerUartStatus::InvalidArgument,
           "rejects a null write buffer for a non-empty request");
    expect(port.read_bytes(buffer.data(), static_cast<std::size_t>(SSIZE_MAX) + 1U).status ==
               ControllerUartStatus::InvalidArgument &&
               port.write_bytes(buffer.data(), static_cast<std::size_t>(SSIZE_MAX) + 1U).status ==
               ControllerUartStatus::InvalidArgument,
           "rejects I/O lengths that exceed the POSIX signed transfer limit");
    expect(port.read_bytes(nullptr, 0U).status == ControllerUartStatus::Success &&
               port.write_bytes(nullptr, 0U).status == ControllerUartStatus::Success,
           "accepts zero-length no-op operations");

    std::array<std::uint8_t, 65536U> large_payload{};
    const auto partial = port.write_bytes(large_payload.data(), large_payload.size());
    expect(partial.status == ControllerUartStatus::Success &&
               partial.bytes_transferred > 0U &&
               partial.bytes_transferred < large_payload.size(),
           "returns the actual count for a possibly partial non-blocking write");

    char temporary_path[] = "/tmp/dockstation-uart-not-a-device-XXXXXX";
    const int temporary_fd = ::mkstemp(temporary_path);
    if (temporary_fd >= 0) {
        (void)::close(temporary_fd);
    }
    ControllerUartPort invalid_device;
    const auto regular_file_result = invalid_device.open(temporary_path);
    (void)::unlink(temporary_path);
    expect(temporary_fd >= 0 && regular_file_result.status == ControllerUartStatus::InvalidDevice,
           "rejects regular files even when the path is absolute");

    port.close();
    port.close();
    expect(!port.is_open() &&
               port.read_bytes(buffer.data(), buffer.size()).status == ControllerUartStatus::NotOpen,
           "close is idempotent and subsequent I/O is rejected");

    ControllerUartPort disconnected;
    expect(disconnected.open(pty.slave_name.c_str()).status == ControllerUartStatus::Success,
           "reopens the pseudo-terminal for disconnect detection");
    (void)::close(pty.master);
    pty.master = -1;
    const auto disconnected_write = disconnected.write_bytes(buffer.data(), buffer.size());
    expect(disconnected_write.status == ControllerUartStatus::Disconnected &&
               !disconnected.is_open(),
           "detects a hard peer disconnect on I/O and closes the stale file descriptor");

    std::cout << "Checks completed: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
