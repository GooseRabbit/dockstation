#include "platform/linux/prototype_uart_telemetry_service.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <string_view>
#include <termios.h>
#include <unistd.h>

namespace dockstation::platform::linux_platform {
namespace {

constexpr std::uint64_t kReconnectIntervalMilliseconds = 2000U;
constexpr std::uint64_t kFreshnessMilliseconds = 6000U;
constexpr std::size_t kMaximumReadsPerPoll = 8U;

std::uint64_t monotonic_milliseconds() noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

std::uint64_t unix_epoch_milliseconds() noexcept {
    const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

bool valid_device_path(std::string_view path) noexcept {
    if (path.size() < 6U || path.size() > 255U || path.substr(0U, 5U) != "/dev/" ||
        path.back() == '/' || path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t offset = 5U;
    while (offset < path.size()) {
        const std::size_t separator = path.find('/', offset);
        const std::size_t end = separator == std::string_view::npos ? path.size() : separator;
        const auto component = path.substr(offset, end - offset);
        if (component.empty() || component == "." || component == "..") return false;
        for (const unsigned char ch : component) {
            const bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
            const bool digit = ch >= '0' && ch <= '9';
            if (!alpha && !digit && ch != '_' && ch != '-' && ch != '.') return false;
        }
        if (separator == std::string_view::npos) break;
        offset = separator + 1U;
    }
    return true;
}

bool parse_unsigned(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_measurement(std::string_view text, double& value, bool& available) noexcept {
    if (text == "NA") {
        value = 0.0;
        available = false;
        return true;
    }
    if (text.empty() || text.size() > 24U) return false;
    std::array<char, 25U> buffer{};
    std::copy(text.begin(), text.end(), buffer.begin());
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(buffer.data(), &end);
    if (errno == ERANGE || end != buffer.data() + text.size() || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    available = true;
    return true;
}

}  // namespace

PrototypeUartTelemetryService::~PrototypeUartTelemetryService() {
    stop();
}

bool PrototypeUartTelemetryService::configure(std::string_view device_path) noexcept {
    stop();
    if (!valid_device_path(device_path)) {
        error_code_ = "INVALID_UART_DEVICE_PATH";
        return false;
    }
    std::copy(device_path.begin(), device_path.end(), device_path_.begin());
    device_path_[device_path.size()] = '\0';
    configured_ = true;
    error_code_ = "UART_DEVICE_WAITING";
    return true;
}

bool PrototypeUartTelemetryService::open_device() noexcept {
    const int descriptor = ::open(
        device_path_.data(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        error_code_ = "UART_DEVICE_OPEN_FAILED";
        return false;
    }

    termios options{};
    if (::tcgetattr(descriptor, &options) != 0) {
        ::close(descriptor);
        error_code_ = "UART_DEVICE_CONFIG_FAILED";
        return false;
    }
    options.c_iflag &= static_cast<tcflag_t>(
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY));
    options.c_oflag &= static_cast<tcflag_t>(~OPOST);
    options.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON | ISIG | IEXTEN));
    options.c_cflag &= static_cast<tcflag_t>(~(PARENB | CSTOPB | CSIZE));
    options.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD);
#ifdef CRTSCTS
    options.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif
    options.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY));
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;
    if (::cfsetispeed(&options, B115200) != 0 ||
        ::cfsetospeed(&options, B115200) != 0 ||
        ::tcsetattr(descriptor, TCSANOW, &options) != 0) {
        ::close(descriptor);
        error_code_ = "UART_DEVICE_CONFIG_FAILED";
        return false;
    }
    (void)::tcflush(descriptor, TCIFLUSH);

    descriptor_ = descriptor;
    input_size_ = 0U;
    error_code_ = "";
    return true;
}

void PrototypeUartTelemetryService::close_device() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    input_size_ = 0U;
}

void PrototypeUartTelemetryService::stop() noexcept {
    close_device();
    device_path_.fill('\0');
    measurements_.fill({});
    sequence_number_ = 0U;
    esp_uptime_ms_ = 0U;
    received_at_unix_ms_ = 0U;
    last_received_monotonic_ms_ = 0U;
    next_open_attempt_ms_ = 0U;
    configured_ = false;
    has_snapshot_ = false;
    error_code_ = "NOT_CONFIGURED";
}

bool PrototypeUartTelemetryService::connected() const noexcept {
    if (descriptor_ < 0 || !has_snapshot_) return false;
    const std::uint64_t now = monotonic_milliseconds();
    return now >= last_received_monotonic_ms_ &&
        now - last_received_monotonic_ms_ <= kFreshnessMilliseconds;
}

const PrototypeUartMeasurement&
PrototypeUartTelemetryService::measurement(std::size_t index) const noexcept {
    static const PrototypeUartMeasurement unavailable{};
    return index < measurements_.size() ? measurements_[index] : unavailable;
}

std::string_view PrototypeUartTelemetryService::device_path() const noexcept {
    return configured_ ? std::string_view{device_path_.data()} : std::string_view{};
}

bool PrototypeUartTelemetryService::process_line(std::string_view line) noexcept {
    // UART2 sends a raw DS1 frame. UART0 carries it inside the standard
    // ESP-IDF console line: "I (123) TAG: DS1,...".
    const std::size_t log_frame = line.find(": DS1,");
    if (log_frame != std::string_view::npos) {
        line.remove_prefix(log_frame + 2U);
    }

    // DS1,<sequence>,<esp_uptime_ms>,<t1>,<h1>,<t2>,<h2>
    std::array<std::string_view, 7U> fields{};
    std::size_t field_index = 0U;
    std::size_t offset = 0U;
    while (offset <= line.size()) {
        if (field_index >= fields.size()) return false;
        const std::size_t separator = line.find(',', offset);
        const std::size_t end = separator == std::string_view::npos ? line.size() : separator;
        fields[field_index++] = line.substr(offset, end - offset);
        if (separator == std::string_view::npos) break;
        offset = separator + 1U;
    }
    if (field_index != fields.size() || fields[0] != "DS1") return false;

    std::uint64_t sequence = 0U;
    std::uint64_t uptime = 0U;
    if (!parse_unsigned(fields[1], sequence) || sequence == 0U ||
        !parse_unsigned(fields[2], uptime)) return false;

    std::array<PrototypeUartMeasurement, 4U> next{};
    for (std::size_t index = 0U; index < next.size(); ++index) {
        bool available = false;
        double value = 0.0;
        if (!parse_measurement(fields[index + 3U], value, available)) return false;
        if (available) {
            const bool temperature = index == 0U || index == 2U;
            if ((temperature && (value < -40.0 || value > 80.0)) ||
                (!temperature && (value < 0.0 || value > 100.0))) return false;
        }
        next[index] = {available, value};
    }

    measurements_ = next;
    sequence_number_ = sequence;  // ESP32 reboot may restart its sequence.
    esp_uptime_ms_ = uptime;
    received_at_unix_ms_ = unix_epoch_milliseconds();
    last_received_monotonic_ms_ = monotonic_milliseconds();
    has_snapshot_ = true;
    error_code_ = "";
    return true;
}

void PrototypeUartTelemetryService::poll() noexcept {
    if (!configured_) return;
    const std::uint64_t now = monotonic_milliseconds();
    if (descriptor_ < 0) {
        if (now < next_open_attempt_ms_) return;
        next_open_attempt_ms_ = now + kReconnectIntervalMilliseconds;
        (void)open_device();
    }
    if (descriptor_ < 0) return;

    std::array<char, 128U> bytes{};
    for (std::size_t iteration = 0U; iteration < kMaximumReadsPerPoll; ++iteration) {
        const ssize_t count = ::read(descriptor_, bytes.data(), bytes.size());
        if (count > 0) {
            for (ssize_t index = 0; index < count; ++index) {
                const char ch = bytes[static_cast<std::size_t>(index)];
                if (ch == '\n') {
                    (void)process_line(std::string_view{input_line_.data(), input_size_});
                    input_size_ = 0U;
                } else if (ch == '\r') {
                    continue;
                } else if (ch == '\0') {
                    input_size_ = 0U;
                } else if (input_size_ < input_line_.size()) {
                    input_line_[input_size_++] = ch;
                } else {
                    input_size_ = 0U;  // Drop oversized frame through next newline.
                }
            }
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_device();
        error_code_ = "UART_DEVICE_READ_FAILED";
        next_open_attempt_ms_ = now + kReconnectIntervalMilliseconds;
        break;
    }
}

}  // namespace dockstation::platform::linux_platform
