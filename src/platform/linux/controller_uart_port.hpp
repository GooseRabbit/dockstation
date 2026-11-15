#pragma once

#include <cstddef>
#include <cstdint>

namespace dockstation::platform::linux_platform {

enum class ControllerUartStatus : std::uint8_t {
    Success,
    WouldBlock,
    NotOpen,
    InvalidArgument,
    InvalidDevice,
    Disconnected,
    Failed
};

struct ControllerUartIoResult final {
    ControllerUartStatus status{ControllerUartStatus::Failed};
    std::size_t bytes_transferred{0U};
    int system_error{0};
};

// Owns one Linux character-device descriptor configured as the Repka side of
// the internal point-to-point UART. This class moves opaque bytes only: callers
// must pass TLS ciphertext, never ControllerCodec frames or plaintext commands.
//
// The object is deliberately non-copyable and non-movable so a single owner
// controls open/close and the file descriptor cannot be duplicated by accident.
class ControllerUartPort final {
public:
    ControllerUartPort() noexcept = default;
    ~ControllerUartPort();

    ControllerUartPort(const ControllerUartPort&) = delete;
    ControllerUartPort& operator=(const ControllerUartPort&) = delete;
    ControllerUartPort(ControllerUartPort&&) = delete;
    ControllerUartPort& operator=(ControllerUartPort&&) = delete;

    // Opens an absolute /dev/... path and configures raw 115200 8N1 operation.
    // The path remains configurable because the final Repka image may expose
    // UART0 under a different stable device name or udev alias.
    [[nodiscard]] ControllerUartIoResult open(const char* absolute_device_path) noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return file_descriptor_ >= 0; }

    // Non-blocking operations. Success may report fewer bytes than requested;
    // the caller must retain the unwritten ciphertext and retry it after a
    // partial write or WouldBlock result.
    // No buffering, polling, retry timer, TLS handling, or frame parsing lives here.
    [[nodiscard]] ControllerUartIoResult read_bytes(
        std::uint8_t* destination, std::size_t destination_capacity) noexcept;
    [[nodiscard]] ControllerUartIoResult write_bytes(
        const std::uint8_t* bytes, std::size_t byte_count) noexcept;

private:
    [[nodiscard]] ControllerUartIoResult classify_io_error(int error_number) noexcept;

    int file_descriptor_{-1};
};

}  // namespace dockstation::platform::linux_platform
