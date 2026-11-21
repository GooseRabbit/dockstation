#include "dht22_gpio_reader.hpp"

#include <array>
#include <cstdint>

#include "esp_timer.h"
#include "esp_rom_sys.h"

namespace dockstation::firmware::esp32 {
namespace {

constexpr std::uint64_t kEdgeTimeoutUs = 120U;
constexpr std::uint32_t kDht22StartPulseUs = 1100U;
constexpr std::uint32_t kDht11StartPulseUs = 20000U;
constexpr std::uint32_t kOnePulseThresholdUs = 50U;

enum class FrameCaptureStatus : std::uint8_t {
    Success,
    NoResponse,
    Timeout,
    GpioError
};

[[nodiscard]] bool wait_for_level(gpio_num_t pin, int expected_level) noexcept {
    const std::int64_t deadline = esp_timer_get_time() +
        static_cast<std::int64_t>(kEdgeTimeoutUs);
    while (esp_timer_get_time() < deadline) {
        if (gpio_get_level(pin) == expected_level) {
            return true;
        }
        esp_rom_delay_us(1U);
    }
    return false;
}

[[nodiscard]] bool capture_high_pulse(
    gpio_num_t pin, std::uint32_t& duration_us) noexcept {
    if (!wait_for_level(pin, 1)) {
        return false;
    }
    const std::int64_t started_at = esp_timer_get_time();
    if (!wait_for_level(pin, 0)) {
        return false;
    }
    const std::int64_t elapsed = esp_timer_get_time() - started_at;
    if (elapsed <= 0 || elapsed > static_cast<std::int64_t>(kEdgeTimeoutUs)) {
        return false;
    }
    duration_us = static_cast<std::uint32_t>(elapsed);
    return true;
}

[[nodiscard]] FrameCaptureStatus capture_frame(
    gpio_num_t pin,
    std::uint32_t start_pulse_us,
    std::array<std::uint8_t, 5U>& frame) noexcept {
    if (gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD) != ESP_OK ||
        gpio_set_level(pin, 0) != ESP_OK) {
        return FrameCaptureStatus::GpioError;
    }

    // DHT22 needs at least 1 ms; DHT11 requires at least 18 ms.
    // The prototype uses 20 ms to keep a margin above the DHT11 minimum.
    esp_rom_delay_us(start_pulse_us);
    if (gpio_set_direction(pin, GPIO_MODE_INPUT) != ESP_OK ||
        gpio_pullup_en(pin) != ESP_OK) {
        return FrameCaptureStatus::GpioError;
    }

    // No low edge means the sensor ignored this start pulse. This is the only
    // case in which auto-detection may safely try the alternate start protocol.
    if (!wait_for_level(pin, 0)) {
        return FrameCaptureStatus::NoResponse;
    }

    // Sensor response: 80 us low, 80 us high, then the first data-bit low.
    if (!wait_for_level(pin, 1) || !wait_for_level(pin, 0)) {
        return FrameCaptureStatus::Timeout;
    }

    frame.fill(0U);
    for (std::size_t bit = 0U; bit < 40U; ++bit) {
        std::uint32_t high_duration_us = 0U;
        if (!capture_high_pulse(pin, high_duration_us)) {
            return FrameCaptureStatus::Timeout;
        }
        const std::size_t byte_index = bit / 8U;
        frame[byte_index] = static_cast<std::uint8_t>(frame[byte_index] << 1U);
        if (high_duration_us > kOnePulseThresholdUs) {
            frame[byte_index] = static_cast<std::uint8_t>(frame[byte_index] | 0x01U);
        }
    }

    return FrameCaptureStatus::Success;
}

}  // namespace

DhtSensorGpioReadStatus read_dht_sensor_gpio(
    gpio_num_t pin, Dht22DecodedReading& output) noexcept {
    output.clear();
    if (pin == GPIO_NUM_NC || !GPIO_IS_VALID_GPIO(pin)) {
        return DhtSensorGpioReadStatus::NotConfigured;
    }

    std::array<std::uint8_t, 5U> frame{};
    auto capture_status = capture_frame(pin, kDht22StartPulseUs, frame);
    if (capture_status == FrameCaptureStatus::NoResponse) {
        // A DHT11 intentionally ignores the shorter DHT22 start pulse. Retry
        // once with its longer request; a DHT22 response/partial frame never
        // triggers this retry, preserving the sensor's minimum read interval.
        capture_status = capture_frame(pin, kDht11StartPulseUs, frame);
    }

    if (capture_status == FrameCaptureStatus::GpioError) {
        return DhtSensorGpioReadStatus::GpioError;
    }
    if (capture_status == FrameCaptureStatus::NoResponse) {
        return DhtSensorGpioReadStatus::NoResponse;
    }
    if (capture_status == FrameCaptureStatus::Timeout) {
        return DhtSensorGpioReadStatus::Timeout;
    }
    if (capture_status != FrameCaptureStatus::Success) {
        return DhtSensorGpioReadStatus::GpioError;
    }

    if (decode_dht22_frame(frame.data(), frame.size(), output) ==
        Dht22DecodeStatus::Success) {
        return DhtSensorGpioReadStatus::Success;
    }
    if (decode_dht11_frame(frame.data(), frame.size(), output) ==
        Dht22DecodeStatus::Success) {
        return DhtSensorGpioReadStatus::Success;
    }
    return DhtSensorGpioReadStatus::InvalidFrame;
}

}  // namespace dockstation::firmware::esp32
