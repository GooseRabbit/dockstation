#include "platform/linux/controller_uart_port.hpp"

#include <cerrno>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace dockstation::platform::linux_platform {
namespace {

[[nodiscard]] ControllerUartIoResult make_result(
    ControllerUartStatus status, int system_error = 0) noexcept {
    return {status, 0U, system_error};
}

[[nodiscard]] bool is_disconnection_error(int error_number) noexcept {
    return error_number == EIO || error_number == ENODEV ||
           error_number == ENXIO || error_number == EPIPE ||
           error_number == EBADF;
}

[[nodiscard]] bool is_busy_error(int error_number) noexcept {
    return error_number == EBUSY || error_number == EAGAIN ||
           error_number == EWOULDBLOCK;
}

}  // namespace

ControllerUartPort::~ControllerUartPort() {
    close();
}

ControllerUartIoResult ControllerUartPort::open(
    const char* absolute_device_path) noexcept {
    close();

    if (absolute_device_path == nullptr || absolute_device_path[0] != '/') {
        return make_result(ControllerUartStatus::InvalidArgument);
    }

    const int descriptor = ::open(
        absolute_device_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        const int error_number = errno;
        return make_result(is_busy_error(error_number)
                               ? ControllerUartStatus::Busy
                               : ControllerUartStatus::Failed,
                           error_number);
    }

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) {
        const int error_number = errno;
        (void)::close(descriptor);
        return make_result(ControllerUartStatus::Failed, error_number);
    }
    if (!S_ISCHR(metadata.st_mode)) {
        (void)::close(descriptor);
        return make_result(ControllerUartStatus::InvalidDevice);
    }

    // flock closes the race between two DockStation instances: the first
    // opener holds an exclusive advisory lock on this device inode. TIOCEXCL
    // then asks the kernel to reject subsequent opens by non-cooperating tools.
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int error_number = errno;
        (void)::close(descriptor);
        return make_result(is_busy_error(error_number)
                               ? ControllerUartStatus::Busy
                               : ControllerUartStatus::Failed,
                           error_number);
    }
    if (::ioctl(descriptor, TIOCEXCL) != 0) {
        const int error_number = errno;
        (void)::close(descriptor);  // Closing also releases the flock.
        return make_result(is_busy_error(error_number)
                               ? ControllerUartStatus::Busy
                               : ControllerUartStatus::Failed,
                           error_number);
    }

    struct termios settings {};
    if (::tcgetattr(descriptor, &settings) != 0) {
        const int error_number = errno;
        (void)::ioctl(descriptor, TIOCNXCL);
        (void)::close(descriptor);
        return make_result(ControllerUartStatus::Failed, error_number);
    }

    // Use raw mode: UART bytes are an opaque TLS record stream, not terminal
    // text. Disable software/hardware flow control and configure exactly 8N1.
    ::cfmakeraw(&settings);
    settings.c_cflag &= static_cast<tcflag_t>(~(PARENB | CSTOPB | CSIZE | CRTSCTS));
    settings.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD | CS8);
    settings.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY));
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;

    if (::cfsetispeed(&settings, B115200) != 0 ||
        ::cfsetospeed(&settings, B115200) != 0 ||
        ::tcsetattr(descriptor, TCSANOW, &settings) != 0) {
        const int error_number = errno;
        (void)::ioctl(descriptor, TIOCNXCL);
        (void)::close(descriptor);
        return make_result(ControllerUartStatus::Failed, error_number);
    }

    file_descriptor_ = descriptor;
    return make_result(ControllerUartStatus::Success);
}

void ControllerUartPort::close() noexcept {
    if (file_descriptor_ >= 0) {
        // The kernel releases TIOCEXCL when the last descriptor for this TTY
        // closes; close also releases the advisory flock atomically.
        (void)::close(file_descriptor_);
        file_descriptor_ = -1;
    }
}

ControllerUartIoResult ControllerUartPort::read_bytes(
    std::uint8_t* destination, std::size_t destination_capacity) noexcept {
    if (file_descriptor_ < 0) {
        return make_result(ControllerUartStatus::NotOpen);
    }
    if (destination_capacity > static_cast<std::size_t>(SSIZE_MAX) ||
        (destination_capacity > 0U && destination == nullptr)) {
        return make_result(ControllerUartStatus::InvalidArgument);
    }
    if (destination_capacity == 0U) {
        return make_result(ControllerUartStatus::Success);
    }

    ssize_t transferred = -1;
    do {
        transferred = ::read(file_descriptor_, destination, destination_capacity);
    } while (transferred < 0 && errno == EINTR);

    if (transferred > 0) {
        return {ControllerUartStatus::Success,
                static_cast<std::size_t>(transferred), 0};
    }
    if (transferred == 0) {
        return make_result(ControllerUartStatus::WouldBlock);
    }

    const int error_number = errno;
    return classify_io_error(error_number);
}

ControllerUartIoResult ControllerUartPort::write_bytes(
    const std::uint8_t* bytes, std::size_t byte_count) noexcept {
    if (file_descriptor_ < 0) {
        return make_result(ControllerUartStatus::NotOpen);
    }
    if (byte_count > static_cast<std::size_t>(SSIZE_MAX) ||
        (byte_count > 0U && bytes == nullptr)) {
        return make_result(ControllerUartStatus::InvalidArgument);
    }
    if (byte_count == 0U) {
        return make_result(ControllerUartStatus::Success);
    }

    ssize_t transferred = -1;
    do {
        transferred = ::write(file_descriptor_, bytes, byte_count);
    } while (transferred < 0 && errno == EINTR);

    if (transferred > 0) {
        return {ControllerUartStatus::Success,
                static_cast<std::size_t>(transferred), 0};
    }
    if (transferred == 0) {
        return make_result(ControllerUartStatus::WouldBlock);
    }

    const int error_number = errno;
    return classify_io_error(error_number);
}

ControllerUartIoResult ControllerUartPort::classify_io_error(
    int error_number) noexcept {
    if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
        return make_result(ControllerUartStatus::WouldBlock, error_number);
    }
    if (is_disconnection_error(error_number)) {
        close();
        return make_result(ControllerUartStatus::Disconnected, error_number);
    }
    return make_result(ControllerUartStatus::Failed, error_number);
}

}  // namespace dockstation::platform::linux_platform
