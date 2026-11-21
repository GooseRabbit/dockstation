#pragma once

#include "dht22_frame_decoder.hpp"
#include "driver/gpio.h"

namespace dockstation::firmware::esp32 {

enum class DhtSensorGpioReadStatus : std::uint8_t {
    Success,
    NotConfigured,
    GpioError,
    NoResponse,
    Timeout,
    InvalidFrame
};

// Automatically reads either DHT22 or DHT11. It tries the DHT22 1.1 ms start
// pulse first, then retries with the DHT11 20 ms pulse only when no sensor
// response was seen. Both variants share the 40-bit transfer and checksum.
// NoResponse means neither request got a response edge; Timeout means a
// response began but its pulse sequence did not complete.
// The data line needs an external 4.7–10 kOhm pull-up; the ESP32 internal
// pull-up is enabled as a fallback. The caller enforces the two-second interval.
[[nodiscard]] DhtSensorGpioReadStatus read_dht_sensor_gpio(
    gpio_num_t pin,
    Dht22DecodedReading& output) noexcept;

}  // namespace dockstation::firmware::esp32
