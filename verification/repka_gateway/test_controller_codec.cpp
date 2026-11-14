#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <variant>

#include "hardware/interfaces/controller_codec.hpp"

using namespace dockstation::hardware::interfaces;

namespace {
int failures = 0;
int checks = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

ControllerMessageEnvelope valid_envelope(ControllerMessageType type) {
    ControllerMessageEnvelope envelope{};
    envelope.type = type;
    envelope.protocol_version = ControllerProtocolVersion{1U, 0U};
    envelope.controller_id = "esp32-controller-01";
    envelope.controller_instance_id = "boot-session-2026-11-14";
    envelope.sequence_number = 42U;
    envelope.sent_at = "2026-11-14T18:00:00Z";
    return envelope;
}

std::uint32_t test_crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0U; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(data[index]);
        for (std::size_t bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask =
                (crc & 1U) != 0U ? 0xFFFFFFFFU : 0U;
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

void repair_crc(std::uint8_t* frame, std::size_t size) {
    const auto crc = test_crc32(frame, size - 4U);
    frame[size - 4U] = static_cast<std::uint8_t>(crc & 0xFFU);
    frame[size - 3U] = static_cast<std::uint8_t>((crc >> 8U) & 0xFFU);
    frame[size - 2U] = static_cast<std::uint8_t>((crc >> 16U) & 0xFFU);
    frame[size - 1U] = static_cast<std::uint8_t>((crc >> 24U) & 0xFFU);
}

ControllerCodecResult encode(
    const ControllerMessageEnvelope& envelope,
    const ControllerMessagePayload& payload,
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize>& frame) {
    return encode_controller_message(
        envelope, payload, frame.data(), frame.size());
}
}  // namespace

int main() {
    std::array<std::uint8_t, kControllerCodecMaximumFrameSize> frame{};

    // A small deterministic frame verifies the header and complete round trip.
    const auto hello_envelope = valid_envelope(ControllerMessageType::Hello);
    const ControllerMessagePayload empty_payload{EmptyControllerMessagePayload{}};
    auto result = encode(hello_envelope, empty_payload, frame);
    expect(result.status == ControllerCodecStatus::Success && result.frame_size > 10U,
           "encodes a complete HELLO frame");
    expect(frame[0] == 0x44U && frame[1] == 0x53U &&
               frame[2] == kControllerCodecWireVersion && frame[3] == 1U,
           "writes the DS magic, wire version and stable HELLO type identifier");

    ControllerCodecDecodeStorage storage{};
    ControllerCodecDecodedMessage decoded{};
    auto decode_result = decode_controller_message(
        frame.data(), result.frame_size, storage, decoded);
    expect(decode_result.status == ControllerCodecStatus::Success,
           "decodes a complete HELLO frame");
    expect(decoded.envelope.type == ControllerMessageType::Hello &&
               decoded.envelope.protocol_version.has_value() &&
               decoded.envelope.protocol_version->major == 1U &&
               decoded.envelope.protocol_version->minor == 0U &&
               decoded.envelope.controller_id == hello_envelope.controller_id &&
               decoded.envelope.controller_instance_id ==
                   hello_envelope.controller_instance_id &&
               decoded.envelope.sequence_number ==
                   hello_envelope.sequence_number &&
               decoded.envelope.sent_at == hello_envelope.sent_at,
           "round-trips every common envelope field");
    expect(std::holds_alternative<EmptyControllerMessagePayload>(decoded.payload),
           "round-trips the empty HELLO body");

    // Telemetry exercises every supported value representation and quality.
    const std::array<ControllerTelemetryMeasurement, 5U> measurements{{
        {"temperature", 23.75, "degC", SensorQualityStatus::Ok, "m-01"},
        {"lid.closed", true, "bool", SensorQualityStatus::Unknown, "m-02"},
        {"error.counter", std::int64_t{-9223372036854775807LL - 1LL},
         "count", SensorQualityStatus::Stale, "m-03"},
        {"bytes.total", std::uint64_t{18446744073709551615ULL},
         "bytes", SensorQualityStatus::Invalid, "m-04"},
        {"weather.mode", std::string_view{"AUTO"}, "state",
         SensorQualityStatus::NotConfigured, "m-05"}
    }};
    const ControllerMessageEnvelope telemetry_envelope =
        valid_envelope(ControllerMessageType::TelemetrySnapshot);
    const ControllerMessagePayload telemetry_payload{
        ControllerTelemetrySnapshotPayload{measurements.data(), measurements.size()}};
    result = encode(telemetry_envelope, telemetry_payload, frame);
    expect(result.status == ControllerCodecStatus::Success,
           "encodes telemetry with all five supported value types/status forms");

    storage = {};
    decoded = {};
    decode_result = decode_controller_message(
        frame.data(), result.frame_size, storage, decoded);
    expect(decode_result.status == ControllerCodecStatus::Success,
           "decodes a multi-measurement telemetry frame");
    const auto* decoded_snapshot =
        std::get_if<ControllerTelemetrySnapshotPayload>(&decoded.payload);
    expect(decoded_snapshot != nullptr &&
               decoded_snapshot->measurement_count == measurements.size() &&
               decoded_snapshot->measurements == storage.measurements.data(),
           "uses caller-provided fixed measurement storage");
    if (decoded_snapshot != nullptr &&
        decoded_snapshot->measurement_count == measurements.size()) {
        const auto& values = storage.measurements;
        const auto* temperature = std::get_if<double>(&values[0].value);
        expect(values[0].sensor_id == "temperature" &&
                   temperature != nullptr && *temperature == 23.75 &&
                   values[0].unit == "degC" &&
                   values[0].quality == SensorQualityStatus::Ok,
               "round-trips binary64 sensor measurements");
        const auto* lid_closed = std::get_if<bool>(&values[1].value);
        expect(values[1].sensor_id == "lid.closed" &&
                   lid_closed != nullptr && *lid_closed &&
                   values[1].quality == SensorQualityStatus::Unknown,
               "round-trips boolean values and UNKNOWN quality");
        const auto* signed_value = std::get_if<std::int64_t>(&values[2].value);
        expect(signed_value != nullptr &&
                   *signed_value == std::numeric_limits<std::int64_t>::min(),
               "round-trips the minimum signed 64-bit value");
        const auto* unsigned_value = std::get_if<std::uint64_t>(&values[3].value);
        expect(unsigned_value != nullptr &&
                   *unsigned_value == std::numeric_limits<std::uint64_t>::max(),
               "round-trips the maximum unsigned 64-bit value");
        const auto* text_value =
            std::get_if<std::string_view>(&values[4].value);
        expect(text_value != nullptr && *text_value == "AUTO" &&
                   values[4].quality == SensorQualityStatus::NotConfigured,
               "round-trips text values and NOT_CONFIGURED quality");
    }

    // A zero-measurement snapshot is valid and has no hidden default data.
    result = encode(
        valid_envelope(ControllerMessageType::TelemetrySnapshot),
        ControllerMessagePayload{ControllerTelemetrySnapshotPayload{}}, frame);
    expect(result.status == ControllerCodecStatus::Success,
           "encodes an empty telemetry snapshot");
    storage = {};
    decoded = {};
    decode_result = decode_controller_message(
        frame.data(), result.frame_size, storage, decoded);
    const auto* empty_snapshot =
        std::get_if<ControllerTelemetrySnapshotPayload>(&decoded.payload);
    expect(decode_result.status == ControllerCodecStatus::Success &&
               empty_snapshot != nullptr &&
               empty_snapshot->measurement_count == 0U,
           "decodes an empty telemetry snapshot");

    // ERROR covers the optional reason field.
    const ControllerErrorPayload error_payload{
        "SENSOR_READ_FAILED", ControllerErrorSeverity::Warning,
        "The temperature sensor did not respond", "No response before timeout"};
    result = encode(
        valid_envelope(ControllerMessageType::Error),
        ControllerMessagePayload{error_payload}, frame);
    expect(result.status == ControllerCodecStatus::Success,
           "encodes a structured ERROR message");
    storage = {};
    decoded = {};
    decode_result = decode_controller_message(
        frame.data(), result.frame_size, storage, decoded);
    const auto* decoded_error = std::get_if<ControllerErrorPayload>(&decoded.payload);
    expect(decode_result.status == ControllerCodecStatus::Success &&
               decoded_error != nullptr &&
               decoded_error->error_code == error_payload.error_code &&
               decoded_error->severity == error_payload.severity &&
               decoded_error->description == error_payload.description &&
               decoded_error->reason_text == error_payload.reason_text,
           "round-trips ERROR fields including an optional known reason");

    // Fixed bounds and invalid caller inputs fail closed.
    std::array<std::uint8_t, 8U> small_buffer{};
    auto small_result = encode_controller_message(
        hello_envelope, empty_payload, small_buffer.data(), small_buffer.size());
    expect(small_result.status == ControllerCodecStatus::BufferTooSmall,
           "rejects an output buffer smaller than the frame header and CRC");
    expect(encode_controller_message(
               hello_envelope, empty_payload, nullptr, frame.size()).status ==
               ControllerCodecStatus::NullBuffer,
           "rejects a null output buffer");

    auto invalid_envelope = hello_envelope;
    invalid_envelope.controller_id = {};
    expect(encode(invalid_envelope, empty_payload, frame).status ==
               ControllerCodecStatus::InvalidEnvelope,
           "rejects an invalid logical envelope before writing a frame");

    const std::string_view invalid_utf8{"\xC3\x28", 2U};
    invalid_envelope = hello_envelope;
    invalid_envelope.controller_id = invalid_utf8;
    expect(encode(invalid_envelope, empty_payload, frame).status ==
               ControllerCodecStatus::InvalidUtf8,
           "rejects malformed UTF-8 instead of transmitting ambiguous text");

    std::array<ControllerTelemetryMeasurement,
               kControllerCodecMaximumMeasurements + 1U> too_many{};
    for (std::size_t index = 0U; index < too_many.size(); ++index) {
        too_many[index] = ControllerTelemetryMeasurement{
            "sensor", std::uint64_t{1U}, "count", SensorQualityStatus::Ok, "time"};
    }
    const auto too_many_payload = ControllerMessagePayload{
        ControllerTelemetrySnapshotPayload{too_many.data(), too_many.size()}};
    expect(encode(valid_envelope(ControllerMessageType::TelemetrySnapshot),
                  too_many_payload, frame).status ==
               ControllerCodecStatus::TooManyMeasurements,
           "rejects more measurements than fixed decode storage can hold");

    // Damage and malformed outer framing are detected before payload use.
    result = encode(hello_envelope, empty_payload, frame);
    auto damaged = frame;
    damaged[result.frame_size - 1U] ^= 0x80U;
    expect(decode_controller_message(
               damaged.data(), result.frame_size, storage, decoded).status ==
               ControllerCodecStatus::CrcMismatch,
           "rejects a frame with a damaged CRC");

    damaged = frame;
    damaged[0] = 0x00U;
    expect(decode_controller_message(
               damaged.data(), result.frame_size, storage, decoded).status ==
               ControllerCodecStatus::BadMagic,
           "rejects an invalid frame signature");

    damaged = frame;
    damaged[2] = static_cast<std::uint8_t>(kControllerCodecWireVersion + 1U);
    expect(decode_controller_message(
               damaged.data(), result.frame_size, storage, decoded).status ==
               ControllerCodecStatus::UnsupportedWireVersion,
           "rejects an unsupported wire format version");

    damaged = frame;
    damaged[3] = 0xFFU;
    expect(decode_controller_message(
               damaged.data(), result.frame_size, storage, decoded).status ==
               ControllerCodecStatus::UnknownMessageType,
           "rejects an unknown message type identifier");

    damaged = frame;
    damaged[4] ^= 0x01U;
    expect(decode_controller_message(
               damaged.data(), result.frame_size, storage, decoded).status ==
               ControllerCodecStatus::PayloadLengthMismatch,
           "rejects a body length inconsistent with the frame size");

    expect(decode_controller_message(
               frame.data(), result.frame_size - 1U, storage, decoded).status ==
               ControllerCodecStatus::PayloadLengthMismatch,
           "rejects a truncated frame");
    damaged = frame;
    damaged[result.frame_size] = 0x00U;
    expect(decode_controller_message(
               damaged.data(), result.frame_size + 1U, storage, decoded).status ==
               ControllerCodecStatus::PayloadLengthMismatch,
           "rejects trailing bytes instead of silently accepting two frames as one");

    std::array<std::uint8_t, kControllerCodecMaximumFrameSize + 1U> oversized{};
    expect(decode_controller_message(
               oversized.data(), oversized.size(), storage, decoded).status ==
               ControllerCodecStatus::FrameTooLarge,
           "rejects frames larger than the documented fixed maximum");

    expect(decode_controller_message(
               nullptr, result.frame_size, storage, decoded).status ==
               ControllerCodecStatus::NullBuffer,
           "rejects a null input buffer");
    expect(decode_controller_message(
               frame.data(), 1U, storage, decoded).status ==
               ControllerCodecStatus::InvalidFrameSize,
           "rejects frames shorter than the minimum framing size");

    // The CRC is not an authentication mechanism: this frame can be modified
    // and checksummed again; semantic validation must still reject bad values.
    const ControllerTelemetryMeasurement boolean_measurement{
        "lid.closed", true, "bool", SensorQualityStatus::Ok, "time"};
    result = encode(
        valid_envelope(ControllerMessageType::TelemetrySnapshot),
        ControllerMessagePayload{ControllerTelemetrySnapshotPayload{
            &boolean_measurement, 1U}}, frame);
    const std::size_t boolean_value_offset =
        6U + 2U + 2U +
        2U + telemetry_envelope.controller_id.size() +
        2U + telemetry_envelope.controller_instance_id.size() +
        8U + 2U + telemetry_envelope.sent_at.size() +
        1U + 2U + boolean_measurement.sensor_id.size() + 1U;
    damaged = frame;
    damaged[boolean_value_offset] = 0x02U;
    repair_crc(damaged.data(), result.frame_size);
    expect(decode_controller_message(
               damaged.data(), result.frame_size, storage, decoded).status ==
               ControllerCodecStatus::InvalidBooleanValue,
           "rejects boolean encodings other than zero or one even with a recomputed CRC");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
