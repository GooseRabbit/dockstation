#pragma once

#include "dht22_frame_decoder.hpp"
#include "driver/gpio.h"

namespace dockstation::firmware::esp32 {

enum class Dht22GpioReadStatus : std::uint8_t {
    Success,
    NotConfigured,
    GpioError,
    Timeout,
    InvalidFrame
};

// Performs one bounded DHT22 single-wire transaction. The data line needs an
// external 4.7–10 kOhm pull-up; the ESP32 internal pull-up is enabled as a
// fallback. The caller must enforce the sensor's two-second read interval.
[[nodiscard]] Dht22GpioReadStatus read_dht22_gpio(
    gpio_num_t pin,
    Dht22DecodedReading& output) noexcept;

}  // namespace dockstation::firmware::esp32
