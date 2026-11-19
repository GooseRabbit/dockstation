#include "dht22_gpio_reader.hpp"

#include <array>
#include <cstdint>

#include "esp_timer.h"
#include "esp_rom_sys.h"

namespace dockstation::firmware::esp32 {
namespace {

constexpr std::uint64_t kEdgeTimeoutUs = 120U;
constexpr std::uint32_t kStartPulseUs = 1100U;
constexpr std::uint32_t kOnePulseThresholdUs = 50U;

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

}  // namespace

Dht22GpioReadStatus read_dht22_gpio(
    gpio_num_t pin, Dht22DecodedReading& output) noexcept {
    output.clear();
    if (pin == GPIO_NUM_NC || !GPIO_IS_VALID_GPIO(pin)) {
        return Dht22GpioReadStatus::NotConfigured;
    }

    if (gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD) != ESP_OK ||
        gpio_set_level(pin, 0) != ESP_OK) {
        return Dht22GpioReadStatus::GpioError;
    }

    // DHT22 requires a host start-low pulse of at least 1 ms.
    esp_rom_delay_us(kStartPulseUs);
    if (gpio_set_direction(pin, GPIO_MODE_INPUT) != ESP_OK ||
        gpio_pullup_en(pin) != ESP_OK) {
        return Dht22GpioReadStatus::GpioError;
    }

    // Sensor response: 80 us low, 80 us high, then the first data-bit low.
    if (!wait_for_level(pin, 0) ||
        !wait_for_level(pin, 1) ||
        !wait_for_level(pin, 0)) {
        return Dht22GpioReadStatus::Timeout;
    }

    std::array<std::uint8_t, 5U> frame{};
    for (std::size_t bit = 0U; bit < 40U; ++bit) {
        std::uint32_t high_duration_us = 0U;
        if (!capture_high_pulse(pin, high_duration_us)) {
            return Dht22GpioReadStatus::Timeout;
        }
        const std::size_t byte_index = bit / 8U;
        frame[byte_index] = static_cast<std::uint8_t>(frame[byte_index] << 1U);
        if (high_duration_us > kOnePulseThresholdUs) {
            frame[byte_index] = static_cast<std::uint8_t>(frame[byte_index] | 0x01U);
        }
    }

    return decode_dht22_frame(frame.data(), frame.size(), output) ==
            Dht22DecodeStatus::Success
        ? Dht22GpioReadStatus::Success
        : Dht22GpioReadStatus::InvalidFrame;
}

}  // namespace dockstation::firmware::esp32
