#pragma once

#include "dht22_frame_decoder.hpp"
#include "driver/gpio.h"

namespace dockstation::firmware::esp32 {

enum class DhtSensorGpioReadStatus : std::uint8_t {
    Success,
    NotConfigured,
    GpioError,
    Timeout,
    InvalidFrame
};

// Automatically reads either DHT22 or DHT11. It tries the DHT22 1.1 ms start
// pulse first, then retries with the DHT11 >=18 ms pulse only when no sensor
// response was seen. Both variants share the 40-bit transfer and checksum.
// The data line needs an external 4.7–10 kOhm pull-up; the ESP32 internal
// pull-up is enabled as a fallback. The caller enforces the two-second interval.
[[nodiscard]] DhtSensorGpioReadStatus read_dht_sensor_gpio(
    gpio_num_t pin,
    Dht22DecodedReading& output) noexcept;

}  // namespace dockstation::firmware::esp32
