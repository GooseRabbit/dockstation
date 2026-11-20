#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dockstation::platform::linux_platform {

// Plaintext, receive-only DHT22 transport for early hardware prototyping.
// It intentionally has no command path and is not suitable for deployment.
struct PrototypeUartMeasurement final {
    bool available{false};
    double value{0.0};
};

class PrototypeUartTelemetryService final {
public:
    PrototypeUartTelemetryService() noexcept = default;
    ~PrototypeUartTelemetryService();

    PrototypeUartTelemetryService(const PrototypeUartTelemetryService&) = delete;
    PrototypeUartTelemetryService& operator=(const PrototypeUartTelemetryService&) = delete;

    // Configure a /dev/... device path. poll() retries opening a missing device
    // so the read-only Gateway can still start before the UART is connected.
    [[nodiscard]] bool configure(std::string_view device_path) noexcept;
    void poll() noexcept;
    void stop() noexcept;

    [[nodiscard]] bool configured() const noexcept { return configured_; }
    [[nodiscard]] bool has_snapshot() const noexcept { return has_snapshot_; }
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::uint64_t sequence_number() const noexcept { return sequence_number_; }
    [[nodiscard]] std::uint64_t esp_uptime_ms() const noexcept { return esp_uptime_ms_; }
    [[nodiscard]] std::uint64_t received_at_unix_ms() const noexcept { return received_at_unix_ms_; }
    [[nodiscard]] const PrototypeUartMeasurement& measurement(std::size_t index) const noexcept;
    [[nodiscard]] const char* error_code() const noexcept { return error_code_; }
    [[nodiscard]] std::string_view device_path() const noexcept;

private:
    [[nodiscard]] bool open_device() noexcept;
    void close_device() noexcept;
    [[nodiscard]] bool process_line(std::string_view line) noexcept;

    int descriptor_{-1};
    std::array<char, 256U> device_path_{};
    std::array<char, 192U> input_line_{};
    std::size_t input_size_{0U};
    std::array<PrototypeUartMeasurement, 4U> measurements_{};
    std::uint64_t sequence_number_{0U};
    std::uint64_t esp_uptime_ms_{0U};
    std::uint64_t received_at_unix_ms_{0U};
    std::uint64_t last_received_monotonic_ms_{0U};
    std::uint64_t next_open_attempt_ms_{0U};
    bool configured_{false};
    bool has_snapshot_{false};
    const char* error_code_{"NOT_CONFIGURED"};
};

}  // namespace dockstation::platform::linux_platform
