#include "dht22_frame_decoder.hpp"

#include <cstdint>

namespace dockstation::firmware::esp32 {
namespace {

constexpr std::size_t kDht22FrameSize = 5U;
constexpr std::uint16_t kMaximumHumidityTenthsPercent = 999U;
constexpr std::int16_t kMinimumTemperatureTenthsC = -400;
constexpr std::int16_t kMaximumTemperatureTenthsC = 800;

}  // namespace

Dht22DecodeStatus decode_dht22_frame(
    const std::uint8_t* frame,
    std::size_t frame_size,
    Dht22DecodedReading& output) noexcept {
    output.clear();

    if (frame == nullptr || frame_size != kDht22FrameSize) {
        return Dht22DecodeStatus::InvalidArgument;
    }

    const std::uint8_t expected_checksum = static_cast<std::uint8_t>(
        static_cast<std::uint16_t>(frame[0]) +
        static_cast<std::uint16_t>(frame[1]) +
        static_cast<std::uint16_t>(frame[2]) +
        static_cast<std::uint16_t>(frame[3]));
    if (expected_checksum != frame[4]) {
        return Dht22DecodeStatus::ChecksumMismatch;
    }

    const std::uint16_t humidity = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame[0]) << 8U) |
        static_cast<std::uint16_t>(frame[1]));
    if (humidity > kMaximumHumidityTenthsPercent) {
        return Dht22DecodeStatus::HumidityOutOfRange;
    }

    const std::uint16_t encoded_temperature = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame[2]) << 8U) |
        static_cast<std::uint16_t>(frame[3]));
    const std::uint16_t magnitude = static_cast<std::uint16_t>(
        encoded_temperature & 0x7FFFU);
    const auto temperature = static_cast<std::int16_t>(
        (encoded_temperature & 0x8000U) != 0U
            ? -static_cast<std::int32_t>(magnitude)
            : static_cast<std::int32_t>(magnitude));

    if (temperature < kMinimumTemperatureTenthsC ||
        temperature > kMaximumTemperatureTenthsC) {
        return Dht22DecodeStatus::TemperatureOutOfRange;
    }

    output.temperature_tenths_c = temperature;
    output.relative_humidity_tenths_percent = humidity;
    return Dht22DecodeStatus::Success;
}


Dht22DecodeStatus decode_dht11_frame(
    const std::uint8_t* frame,
    std::size_t frame_size,
    Dht22DecodedReading& output) noexcept {
    output.clear();

    if (frame == nullptr || frame_size != kDht22FrameSize) {
        return Dht22DecodeStatus::InvalidArgument;
    }

    const std::uint8_t expected_checksum = static_cast<std::uint8_t>(
        static_cast<std::uint16_t>(frame[0]) +
        static_cast<std::uint16_t>(frame[1]) +
        static_cast<std::uint16_t>(frame[2]) +
        static_cast<std::uint16_t>(frame[3]));
    if (expected_checksum != frame[4]) {
        return Dht22DecodeStatus::ChecksumMismatch;
    }

    // DHT11 transmits integer and decimal bytes separately. Decimal bytes are
    // decimal digits, unlike the raw low bytes used by DHT22.
    if (frame[1] > 9U || frame[3] > 9U) {
        return Dht22DecodeStatus::InvalidArgument;
    }

    const std::uint16_t humidity_tenths = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(frame[0]) * 10U + frame[1]);
    if (humidity_tenths > kMaximumHumidityTenthsPercent) {
        return Dht22DecodeStatus::HumidityOutOfRange;
    }

    const std::uint16_t temperature_tenths = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(frame[2]) * 10U + frame[3]);
    // DHT11's specified operating range is 0.0 ... 50.0 C.
    if (temperature_tenths > 500U) {
        return Dht22DecodeStatus::TemperatureOutOfRange;
    }

    output.temperature_tenths_c = static_cast<std::int16_t>(temperature_tenths);
    output.relative_humidity_tenths_percent = humidity_tenths;
    return Dht22DecodeStatus::Success;
}

}  // namespace dockstation::firmware::esp32
