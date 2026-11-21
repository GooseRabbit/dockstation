#include "dht22_frame_decoder.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

using namespace dockstation::firmware::esp32;

namespace {
int checks = 0;
int failures = 0;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        std::cerr << "FAIL - " << description << '\n';
        ++failures;
    }
}

std::array<std::uint8_t, 5U> make_frame(
    std::uint16_t humidity_tenths,
    std::int16_t temperature_tenths) {
    const std::uint16_t encoded_temperature =
        temperature_tenths < 0
            ? static_cast<std::uint16_t>(
                  0x8000U + static_cast<std::uint16_t>(-temperature_tenths))
            : static_cast<std::uint16_t>(temperature_tenths);

    std::array<std::uint8_t, 5U> frame{{
        static_cast<std::uint8_t>(humidity_tenths >> 8U),
        static_cast<std::uint8_t>(humidity_tenths & 0xFFU),
        static_cast<std::uint8_t>(encoded_temperature >> 8U),
        static_cast<std::uint8_t>(encoded_temperature & 0xFFU),
        0U
    }};
    frame[4] = static_cast<std::uint8_t>(
        static_cast<std::uint16_t>(frame[0]) +
        static_cast<std::uint16_t>(frame[1]) +
        static_cast<std::uint16_t>(frame[2]) +
        static_cast<std::uint16_t>(frame[3]));
    return frame;
}


std::array<std::uint8_t, 5U> make_dht11_frame(
    std::uint8_t humidity_integer,
    std::uint8_t humidity_decimal,
    std::uint8_t temperature_integer,
    std::uint8_t temperature_decimal) {
    std::array<std::uint8_t, 5U> frame{{
        humidity_integer, humidity_decimal, temperature_integer,
        temperature_decimal, 0U
    }};
    frame[4] = static_cast<std::uint8_t>(
        static_cast<std::uint16_t>(frame[0]) +
        static_cast<std::uint16_t>(frame[1]) +
        static_cast<std::uint16_t>(frame[2]) +
        static_cast<std::uint16_t>(frame[3]));
    return frame;
}

void expect_dht11_rejected(
    const std::array<std::uint8_t, 5U>& frame,
    Dht22DecodeStatus expected_status,
    const char* description) {
    Dht22DecodedReading reading{123, 456U};
    const auto status = decode_dht11_frame(frame.data(), frame.size(), reading);
    expect(status == expected_status &&
               reading.temperature_tenths_c == 0 &&
               reading.relative_humidity_tenths_percent == 0U,
           description);
}

void expect_rejected(
    const std::array<std::uint8_t, 5U>& frame,
    Dht22DecodeStatus expected_status,
    const char* description) {
    Dht22DecodedReading reading{123, 456U};
    const auto status = decode_dht22_frame(frame.data(), frame.size(), reading);
    expect(status == expected_status &&
               reading.temperature_tenths_c == 0 &&
               reading.relative_humidity_tenths_percent == 0U,
           description);
}
}  // namespace

int main() {
    Dht22DecodedReading reading{};

    // Manufacturer example: 65.8 %RH and 26.9 °C.
    const std::array<std::uint8_t, 5U> positive_example{{
        0x02U, 0x92U, 0x01U, 0x0DU, 0xA2U
    }};
    expect(decode_dht22_frame(
               positive_example.data(), positive_example.size(), reading) ==
               Dht22DecodeStatus::Success &&
               reading.relative_humidity_tenths_percent == 658U &&
               reading.temperature_tenths_c == 269,
           "manufacturer positive-temperature example decodes exactly");

    // Manufacturer negative-temperature example: -10.1 °C.
    const auto negative_example = make_frame(658U, -101);
    expect(decode_dht22_frame(
               negative_example.data(), negative_example.size(), reading) ==
               Dht22DecodeStatus::Success &&
               reading.relative_humidity_tenths_percent == 658U &&
               reading.temperature_tenths_c == -101,
           "negative temperature sign bit is decoded correctly");

    const auto lower_limits = make_frame(0U, -400);
    expect(decode_dht22_frame(
               lower_limits.data(), lower_limits.size(), reading) ==
               Dht22DecodeStatus::Success &&
               reading.relative_humidity_tenths_percent == 0U &&
               reading.temperature_tenths_c == -400,
           "zero humidity and minimum temperature are accepted");

    const auto upper_limits = make_frame(999U, 800);
    expect(decode_dht22_frame(
               upper_limits.data(), upper_limits.size(), reading) ==
               Dht22DecodeStatus::Success &&
               reading.relative_humidity_tenths_percent == 999U &&
               reading.temperature_tenths_c == 800,
           "maximum humidity and maximum temperature are accepted");

    auto bad_checksum = positive_example;
    bad_checksum[4] ^= 0x01U;
    expect_rejected(
        bad_checksum, Dht22DecodeStatus::ChecksumMismatch,
        "checksum mismatch is rejected and output is cleared");

    const auto excessive_humidity = make_frame(1000U, 250);
    expect_rejected(
        excessive_humidity, Dht22DecodeStatus::HumidityOutOfRange,
        "humidity above the documented 99.9 percent range is rejected");

    const auto excessive_temperature = make_frame(500U, 801);
    expect_rejected(
        excessive_temperature, Dht22DecodeStatus::TemperatureOutOfRange,
        "temperature above 80.0 C is rejected");

    const auto low_temperature = make_frame(500U, -401);
    expect_rejected(
        low_temperature, Dht22DecodeStatus::TemperatureOutOfRange,
        "temperature below -40.0 C is rejected");


    // DHT11 encodes integer and decimal bytes separately, unlike DHT22.
    const auto dht11_integer_example = make_dht11_frame(45U, 0U, 23U, 0U);
    expect(decode_dht11_frame(
               dht11_integer_example.data(), dht11_integer_example.size(), reading) ==
               Dht22DecodeStatus::Success &&
               reading.relative_humidity_tenths_percent == 450U &&
               reading.temperature_tenths_c == 230,
           "DHT11 integer example decodes and is normalized to tenths");

    const auto dht11_decimal_example = make_dht11_frame(45U, 5U, 23U, 4U);
    expect(decode_dht11_frame(
               dht11_decimal_example.data(), dht11_decimal_example.size(), reading) ==
               Dht22DecodeStatus::Success &&
               reading.relative_humidity_tenths_percent == 455U &&
               reading.temperature_tenths_c == 234,
           "DHT11 decimal digits are preserved in tenths");

    auto dht11_bad_checksum = dht11_integer_example;
    dht11_bad_checksum[4] ^= 0x01U;
    expect_dht11_rejected(
        dht11_bad_checksum, Dht22DecodeStatus::ChecksumMismatch,
        "DHT11 checksum mismatch is rejected and output is cleared");

    const auto dht11_bad_humidity_decimal = make_dht11_frame(45U, 10U, 23U, 0U);
    expect_dht11_rejected(
        dht11_bad_humidity_decimal, Dht22DecodeStatus::InvalidArgument,
        "DHT11 humidity decimal byte must be a single decimal digit");

    const auto dht11_bad_temperature_decimal = make_dht11_frame(45U, 0U, 23U, 10U);
    expect_dht11_rejected(
        dht11_bad_temperature_decimal, Dht22DecodeStatus::InvalidArgument,
        "DHT11 temperature decimal byte must be a single decimal digit");

    const auto dht11_excessive_humidity = make_dht11_frame(100U, 0U, 23U, 0U);
    expect_dht11_rejected(
        dht11_excessive_humidity, Dht22DecodeStatus::HumidityOutOfRange,
        "DHT11 humidity above 99.9 percent is rejected");

    const auto dht11_excessive_temperature = make_dht11_frame(45U, 0U, 50U, 1U);
    expect_dht11_rejected(
        dht11_excessive_temperature, Dht22DecodeStatus::TemperatureOutOfRange,
        "DHT11 temperature above 50.0 C is rejected");

    reading = {123, 456U};
    expect(decode_dht22_frame(nullptr, 5U, reading) ==
               Dht22DecodeStatus::InvalidArgument &&
               reading.temperature_tenths_c == 0 &&
               reading.relative_humidity_tenths_percent == 0U,
           "null frame is rejected and output is cleared");

    reading = {123, 456U};
    expect(decode_dht22_frame(
               positive_example.data(), positive_example.size() - 1U,
               reading) == Dht22DecodeStatus::InvalidArgument &&
               reading.temperature_tenths_c == 0 &&
               reading.relative_humidity_tenths_percent == 0U,
           "short frame is rejected and output is cleared");

    reading = {123, 456U};
    expect(decode_dht22_frame(
               positive_example.data(), positive_example.size() + 1U,
               reading) == Dht22DecodeStatus::InvalidArgument &&
               reading.temperature_tenths_c == 0 &&
               reading.relative_humidity_tenths_percent == 0U,
           "oversized frame is rejected and output is cleared");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
