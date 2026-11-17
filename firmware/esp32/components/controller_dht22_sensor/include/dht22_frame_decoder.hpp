#pragma once

#include <cstddef>
#include <cstdint>

namespace dockstation::firmware::esp32 {

// A decoded AM2302/DHT22 reading is kept in tenths to preserve the sensor's
// 0.1-unit resolution without introducing rounding during frame decoding.
struct Dht22DecodedReading final {
    std::int16_t temperature_tenths_c{0};
    std::uint16_t relative_humidity_tenths_percent{0};

    void clear() noexcept {
        temperature_tenths_c = 0;
        relative_humidity_tenths_percent = 0U;
    }
};

enum class Dht22DecodeStatus : std::uint8_t {
    Success,
    InvalidArgument,
    ChecksumMismatch,
    HumidityOutOfRange,
    TemperatureOutOfRange
};

// Decodes the five bytes already captured from an AM2302/DHT22 single-bus
// transfer. This function performs no GPIO access and never invents readings.
// On every failure, output is cleared.
[[nodiscard]] Dht22DecodeStatus decode_dht22_frame(
    const std::uint8_t* frame,
    std::size_t frame_size,
    Dht22DecodedReading& output) noexcept;

}  // namespace dockstation::firmware::esp32
