#include "hardware/interfaces/controller_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <variant>

namespace dockstation::hardware::interfaces {
namespace {

constexpr std::uint8_t kMagicFirst = 0x44U;  // 'D'
constexpr std::uint8_t kMagicSecond = 0x53U; // 'S'
constexpr std::size_t kHeaderSize = 6U;
constexpr std::size_t kCrcSize = 4U;
constexpr std::uint8_t kValueBoolean = 1U;
constexpr std::uint8_t kValueSigned = 2U;
constexpr std::uint8_t kValueUnsigned = 3U;
constexpr std::uint8_t kValueFloat64 = 4U;
constexpr std::uint8_t kValueText = 5U;

static_assert(sizeof(double) == sizeof(std::uint64_t),
              "ControllerCodec requires 64-bit double");
static_assert(std::numeric_limits<double>::is_iec559,
              "ControllerCodec requires IEEE-754 binary64 double");

[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        if (first == 0U) {
            return false;
        }
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        std::uint32_t minimum_code_point = 0U;
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1U;
            code_point = static_cast<std::uint32_t>(first & 0x1FU);
            minimum_code_point = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2U;
            code_point = static_cast<std::uint32_t>(first & 0x0FU);
            minimum_code_point = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3U;
            code_point = static_cast<std::uint32_t>(first & 0x07U);
            minimum_code_point = 0x10000U;
        } else {
            return false;
        }

        if (continuation_count > value.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const auto next = static_cast<std::uint8_t>(value[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) |
                         static_cast<std::uint32_t>(next & 0x3FU);
        }
        if (code_point < minimum_code_point || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

class Writer final {
public:
    Writer(std::uint8_t* data, std::size_t capacity) noexcept
        : data_(data), capacity_(capacity) {}

    [[nodiscard]] bool write_u8(std::uint8_t value) noexcept {
        if (!reserve(1U)) {
            return false;
        }
        data_[offset_] = value;
        ++offset_;
        return true;
    }

    [[nodiscard]] bool write_u16(std::uint16_t value) noexcept {
        return write_u8(static_cast<std::uint8_t>(value & 0xFFU)) &&
               write_u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    }

    [[nodiscard]] bool write_u32(std::uint32_t value) noexcept {
        return write_u8(static_cast<std::uint8_t>(value & 0xFFU)) &&
               write_u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU)) &&
               write_u8(static_cast<std::uint8_t>((value >> 16U) & 0xFFU)) &&
               write_u8(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    }

    [[nodiscard]] bool write_u64(std::uint64_t value) noexcept {
        return write_u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL)) &&
               write_u32(static_cast<std::uint32_t>((value >> 32U) & 0xFFFFFFFFULL));
    }

    [[nodiscard]] bool write_text(std::string_view value) noexcept {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
            status_ = ControllerCodecStatus::StringTooLong;
            return false;
        }
        if (!is_valid_utf8(value)) {
            status_ = ControllerCodecStatus::InvalidUtf8;
            return false;
        }
        if (!write_u16(static_cast<std::uint16_t>(value.size()))) {
            return false;
        }
        return write_bytes(reinterpret_cast<const std::uint8_t*>(value.data()),
                           value.size());
    }

    [[nodiscard]] bool write_bytes(const std::uint8_t* bytes,
                                   std::size_t count) noexcept {
        if (count == 0U) {
            return true;
        }
        if (bytes == nullptr) {
            status_ = ControllerCodecStatus::NullBuffer;
            return false;
        }
        if (!reserve(count)) {
            return false;
        }
        std::memcpy(data_ + offset_, bytes, count);
        offset_ += count;
        return true;
    }

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] ControllerCodecStatus status() const noexcept { return status_; }

private:
    [[nodiscard]] bool reserve(std::size_t count) noexcept {
        if (count > capacity_ - offset_) {
            status_ = ControllerCodecStatus::BufferTooSmall;
            return false;
        }
        return true;
    }

    std::uint8_t* data_{nullptr};
    std::size_t capacity_{0U};
    std::size_t offset_{0U};
    ControllerCodecStatus status_{ControllerCodecStatus::Success};
};

class Reader final {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    [[nodiscard]] bool read_u8(std::uint8_t& value) noexcept {
        if (!has_bytes(1U)) {
            return false;
        }
        value = data_[offset_];
        ++offset_;
        return true;
    }

    [[nodiscard]] bool read_u16(std::uint16_t& value) noexcept {
        std::uint8_t low = 0U;
        std::uint8_t high = 0U;
        if (!read_u8(low) || !read_u8(high)) {
            return false;
        }
        value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(low) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U));
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept {
        std::uint16_t low = 0U;
        std::uint16_t high = 0U;
        if (!read_u16(low) || !read_u16(high)) {
            return false;
        }
        value = static_cast<std::uint32_t>(low) |
                (static_cast<std::uint32_t>(high) << 16U);
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& value) noexcept {
        std::uint32_t low = 0U;
        std::uint32_t high = 0U;
        if (!read_u32(low) || !read_u32(high)) {
            return false;
        }
        value = static_cast<std::uint64_t>(low) |
                (static_cast<std::uint64_t>(high) << 32U);
        return true;
    }

    [[nodiscard]] bool read_text(std::string_view& value) noexcept {
        std::uint16_t length = 0U;
        if (!read_u16(length)) {
            return false;
        }
        const auto count = static_cast<std::size_t>(length);
        if (!has_bytes(count)) {
            return false;
        }
        value = std::string_view{
            reinterpret_cast<const char*>(data_ + offset_), count};
        if (!is_valid_utf8(value)) {
            status_ = ControllerCodecStatus::InvalidUtf8;
            return false;
        }
        offset_ += count;
        return true;
    }

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] ControllerCodecStatus status() const noexcept { return status_; }

private:
    [[nodiscard]] bool has_bytes(std::size_t count) noexcept {
        if (count > size_ - offset_) {
            status_ = ControllerCodecStatus::MalformedField;
            return false;
        }
        return true;
    }

    const std::uint8_t* data_{nullptr};
    std::size_t size_{0U};
    std::size_t offset_{0U};
    ControllerCodecStatus status_{ControllerCodecStatus::Success};
};

[[nodiscard]] std::uint32_t crc32_iso_hdlc(const std::uint8_t* data,
                                           std::size_t size) noexcept {
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

[[nodiscard]] std::uint32_t load_u32_le(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

[[nodiscard]] bool write_message_type(Writer& writer,
                                      ControllerMessageType type) noexcept {
    std::uint8_t wire_type = 0U;
    switch (type) {
        case ControllerMessageType::Hello: wire_type = 1U; break;
        case ControllerMessageType::HelloAck: wire_type = 2U; break;
        case ControllerMessageType::Heartbeat: wire_type = 3U; break;
        case ControllerMessageType::RequestSnapshot: wire_type = 4U; break;
        case ControllerMessageType::TelemetrySnapshot: wire_type = 5U; break;
        case ControllerMessageType::Error: wire_type = 6U; break;
        case ControllerMessageType::Unknown: return false;
    }
    return writer.write_u8(wire_type);
}

[[nodiscard]] bool read_message_type(std::uint8_t wire_type,
                                     ControllerMessageType& type) noexcept {
    switch (wire_type) {
        case 1U: type = ControllerMessageType::Hello; return true;
        case 2U: type = ControllerMessageType::HelloAck; return true;
        case 3U: type = ControllerMessageType::Heartbeat; return true;
        case 4U: type = ControllerMessageType::RequestSnapshot; return true;
        case 5U: type = ControllerMessageType::TelemetrySnapshot; return true;
        case 6U: type = ControllerMessageType::Error; return true;
        default: return false;
    }
}

[[nodiscard]] bool write_sensor_quality(Writer& writer,
                                        SensorQualityStatus quality) noexcept {
    std::uint8_t wire_value = 0U;
    switch (quality) {
        case SensorQualityStatus::Ok: wire_value = 1U; break;
        case SensorQualityStatus::Stale: wire_value = 2U; break;
        case SensorQualityStatus::Invalid: wire_value = 3U; break;
        case SensorQualityStatus::Unavailable: wire_value = 4U; break;
        case SensorQualityStatus::NotConfigured: wire_value = 5U; break;
        case SensorQualityStatus::Unknown: wire_value = 6U; break;
    }
    return writer.write_u8(wire_value);
}

[[nodiscard]] bool read_sensor_quality(Reader& reader,
                                       SensorQualityStatus& quality) noexcept {
    std::uint8_t wire_value = 0U;
    if (!reader.read_u8(wire_value)) {
        return false;
    }
    switch (wire_value) {
        case 1U: quality = SensorQualityStatus::Ok; return true;
        case 2U: quality = SensorQualityStatus::Stale; return true;
        case 3U: quality = SensorQualityStatus::Invalid; return true;
        case 4U: quality = SensorQualityStatus::Unavailable; return true;
        case 5U: quality = SensorQualityStatus::NotConfigured; return true;
        case 6U: quality = SensorQualityStatus::Unknown; return true;
        default: return false;
    }
}

[[nodiscard]] bool write_error_severity(
    Writer& writer, ControllerErrorSeverity severity) noexcept {
    std::uint8_t wire_value = 0U;
    switch (severity) {
        case ControllerErrorSeverity::Info: wire_value = 1U; break;
        case ControllerErrorSeverity::Warning: wire_value = 2U; break;
        case ControllerErrorSeverity::Error: wire_value = 3U; break;
        case ControllerErrorSeverity::Critical: wire_value = 4U; break;
        case ControllerErrorSeverity::Unknown: return false;
    }
    return writer.write_u8(wire_value);
}

[[nodiscard]] bool read_error_severity(
    Reader& reader, ControllerErrorSeverity& severity) noexcept {
    std::uint8_t wire_value = 0U;
    if (!reader.read_u8(wire_value)) {
        return false;
    }
    switch (wire_value) {
        case 1U: severity = ControllerErrorSeverity::Info; return true;
        case 2U: severity = ControllerErrorSeverity::Warning; return true;
        case 3U: severity = ControllerErrorSeverity::Error; return true;
        case 4U: severity = ControllerErrorSeverity::Critical; return true;
        default: return false;
    }
}

[[nodiscard]] bool write_sensor_value(
    Writer& writer, const ControllerSensorValue& value) noexcept {
    if (value.valueless_by_exception()) {
        return false;
    }
    return std::visit(
        [&writer](const auto& item) noexcept -> bool {
            using ValueType = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<ValueType, bool>) {
                return writer.write_u8(kValueBoolean) &&
                       writer.write_u8(item ? 1U : 0U);
            } else if constexpr (std::is_same_v<ValueType, std::int64_t>) {
                return writer.write_u8(kValueSigned) &&
                       writer.write_u64(static_cast<std::uint64_t>(item));
            } else if constexpr (std::is_same_v<ValueType, std::uint64_t>) {
                return writer.write_u8(kValueUnsigned) &&
                       writer.write_u64(item);
            } else if constexpr (std::is_same_v<ValueType, double>) {
                std::uint64_t bits = 0U;
                std::memcpy(&bits, &item, sizeof(bits));
                return writer.write_u8(kValueFloat64) && writer.write_u64(bits);
            } else {
                return writer.write_u8(kValueText) && writer.write_text(item);
            }
        },
        value);
}

[[nodiscard]] bool read_sensor_value(
    Reader& reader, ControllerSensorValue& value,
    ControllerCodecStatus& status) noexcept {
    std::uint8_t wire_type = 0U;
    if (!reader.read_u8(wire_type)) {
        status = reader.status();
        return false;
    }
    switch (wire_type) {
        case kValueBoolean: {
            std::uint8_t boolean_value = 0U;
            if (!reader.read_u8(boolean_value)) {
                status = reader.status();
                return false;
            }
            if (boolean_value > 1U) {
                status = ControllerCodecStatus::InvalidBooleanValue;
                return false;
            }
            value = boolean_value == 1U;
            return true;
        }
        case kValueSigned: {
            std::uint64_t raw = 0U;
            if (!reader.read_u64(raw)) {
                status = reader.status();
                return false;
            }
            std::int64_t signed_value = 0;
            if (raw <= static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
                signed_value = static_cast<std::int64_t>(raw);
            } else {
                const auto inverted = ~raw;
                signed_value = -1 - static_cast<std::int64_t>(inverted);
            }
            value = signed_value;
            return true;
        }
        case kValueUnsigned: {
            std::uint64_t unsigned_value = 0U;
            if (!reader.read_u64(unsigned_value)) {
                status = reader.status();
                return false;
            }
            value = unsigned_value;
            return true;
        }
        case kValueFloat64: {
            std::uint64_t raw = 0U;
            if (!reader.read_u64(raw)) {
                status = reader.status();
                return false;
            }
            double double_value = 0.0;
            std::memcpy(&double_value, &raw, sizeof(double_value));
            value = double_value;
            return true;
        }
        case kValueText: {
            std::string_view text_value{};
            if (!reader.read_text(text_value)) {
                status = reader.status();
                return false;
            }
            value = text_value;
            return true;
        }
        default:
            status = ControllerCodecStatus::UnsupportedValueType;
            return false;
    }
}

[[nodiscard]] bool write_payload(Writer& writer,
                                 ControllerMessageType type,
                                 const ControllerMessagePayload& payload,
                                 ControllerCodecStatus& status) noexcept {
    switch (type) {
        case ControllerMessageType::Hello:
        case ControllerMessageType::HelloAck:
        case ControllerMessageType::Heartbeat:
        case ControllerMessageType::RequestSnapshot:
            return std::holds_alternative<EmptyControllerMessagePayload>(payload);

        case ControllerMessageType::TelemetrySnapshot: {
            const auto* snapshot =
                std::get_if<ControllerTelemetrySnapshotPayload>(&payload);
            if (snapshot == nullptr) {
                return false;
            }
            if (snapshot->measurement_count > kControllerCodecMaximumMeasurements) {
                status = ControllerCodecStatus::TooManyMeasurements;
                return false;
            }
            if (!writer.write_u8(
                    static_cast<std::uint8_t>(snapshot->measurement_count))) {
                status = writer.status();
                return false;
            }
            for (std::size_t index = 0U;
                 index < snapshot->measurement_count; ++index) {
                const auto& measurement = snapshot->measurements[index];
                if (!writer.write_text(measurement.sensor_id) ||
                    !write_sensor_value(writer, measurement.value) ||
                    !writer.write_text(measurement.unit) ||
                    !write_sensor_quality(writer, measurement.quality) ||
                    !writer.write_text(measurement.measured_at)) {
                    status = writer.status();
                    if (status == ControllerCodecStatus::Success) {
                        status = ControllerCodecStatus::InvalidPayload;
                    }
                    return false;
                }
            }
            return true;
        }

        case ControllerMessageType::Error: {
            const auto* error = std::get_if<ControllerErrorPayload>(&payload);
            if (error == nullptr) {
                return false;
            }
            if (!writer.write_text(error->error_code) ||
                !write_error_severity(writer, error->severity) ||
                !writer.write_text(error->description) ||
                !writer.write_text(error->reason_text)) {
                status = writer.status();
                if (status == ControllerCodecStatus::Success) {
                    status = ControllerCodecStatus::InvalidPayload;
                }
                return false;
            }
            return true;
        }

        case ControllerMessageType::Unknown:
            return false;
    }
    return false;
}

[[nodiscard]] bool read_payload(
    Reader& reader, ControllerMessageType type,
    ControllerCodecDecodeStorage& storage,
    ControllerMessagePayload& payload,
    ControllerCodecStatus& status) noexcept {
    switch (type) {
        case ControllerMessageType::Hello:
        case ControllerMessageType::HelloAck:
        case ControllerMessageType::Heartbeat:
        case ControllerMessageType::RequestSnapshot:
            payload = EmptyControllerMessagePayload{};
            return true;

        case ControllerMessageType::TelemetrySnapshot: {
            std::uint8_t count = 0U;
            if (!reader.read_u8(count)) {
                status = reader.status();
                return false;
            }
            if (static_cast<std::size_t>(count) >
                kControllerCodecMaximumMeasurements) {
                status = ControllerCodecStatus::TooManyMeasurements;
                return false;
            }
            for (std::size_t index = 0U;
                 index < static_cast<std::size_t>(count); ++index) {
                auto& measurement = storage.measurements[index];
                if (!reader.read_text(measurement.sensor_id) ||
                    !read_sensor_value(reader, measurement.value, status) ||
                    !reader.read_text(measurement.unit) ||
                    !read_sensor_quality(reader, measurement.quality) ||
                    !reader.read_text(measurement.measured_at)) {
                    if (status == ControllerCodecStatus::Success) {
                        status = reader.status();
                    }
                    if (status == ControllerCodecStatus::Success) {
                        status = ControllerCodecStatus::MalformedField;
                    }
                    return false;
                }
            }
            payload = ControllerTelemetrySnapshotPayload{
                storage.measurements.data(), static_cast<std::size_t>(count)};
            return true;
        }

        case ControllerMessageType::Error: {
            ControllerErrorPayload error{};
            if (!reader.read_text(error.error_code) ||
                !read_error_severity(reader, error.severity) ||
                !reader.read_text(error.description) ||
                !reader.read_text(error.reason_text)) {
                status = reader.status();
                if (status == ControllerCodecStatus::Success) {
                    status = ControllerCodecStatus::MalformedField;
                }
                return false;
            }
            payload = error;
            return true;
        }

        case ControllerMessageType::Unknown:
            status = ControllerCodecStatus::UnknownMessageType;
            return false;
    }
    status = ControllerCodecStatus::UnknownMessageType;
    return false;
}

[[nodiscard]] ControllerCodecResult failure(
    ControllerCodecStatus status, std::size_t offset = 0U) noexcept {
    return {status, 0U, offset};
}

}  // namespace

ControllerCodecResult encode_controller_message(
    const ControllerMessageEnvelope& envelope,
    const ControllerMessagePayload& payload,
    std::uint8_t* destination,
    std::size_t destination_capacity) noexcept {
    if (destination == nullptr) {
        return failure(ControllerCodecStatus::NullBuffer);
    }
    if (destination_capacity < kHeaderSize + kCrcSize) {
        return failure(ControllerCodecStatus::BufferTooSmall);
    }
    if (!is_known_controller_message_type(envelope.type)) {
        return failure(ControllerCodecStatus::UnknownMessageType);
    }
    if (validate_controller_message(envelope).status !=
        ControllerMessageValidationStatus::Valid) {
        return failure(ControllerCodecStatus::InvalidEnvelope);
    }
    if (envelope.type == ControllerMessageType::TelemetrySnapshot) {
        const auto* snapshot = std::get_if<ControllerTelemetrySnapshotPayload>(&payload);
        if (snapshot != nullptr &&
            snapshot->measurement_count > kControllerCodecMaximumMeasurements) {
            return failure(ControllerCodecStatus::TooManyMeasurements);
        }
    }
    if (validate_controller_message_payload(envelope.type, payload).status !=
        ControllerPayloadValidationStatus::Valid) {
        return failure(ControllerCodecStatus::InvalidPayload);
    }

    const auto effective_capacity =
        std::min(destination_capacity, kControllerCodecMaximumFrameSize);
    Writer writer(destination, effective_capacity);
    if (!writer.write_u8(kMagicFirst) ||
        !writer.write_u8(kMagicSecond) ||
        !writer.write_u8(kControllerCodecWireVersion) ||
        !write_message_type(writer, envelope.type) ||
        !writer.write_u16(0U)) {
        return failure(writer.status(), writer.offset());
    }

    const auto body_start = writer.offset();
    const auto major = envelope.protocol_version->major;
    const auto minor = envelope.protocol_version->minor;
    if (!writer.write_u16(major) || !writer.write_u16(minor) ||
        !writer.write_text(envelope.controller_id) ||
        !writer.write_text(envelope.controller_instance_id) ||
        !writer.write_u64(*envelope.sequence_number) ||
        !writer.write_text(envelope.sent_at)) {
        return failure(writer.status(), writer.offset());
    }

    auto payload_status = ControllerCodecStatus::Success;
    if (!write_payload(writer, envelope.type, payload, payload_status)) {
        const auto status = payload_status != ControllerCodecStatus::Success
            ? payload_status
            : (writer.status() != ControllerCodecStatus::Success
                   ? writer.status()
                   : ControllerCodecStatus::InvalidPayload);
        return failure(status, writer.offset());
    }

    const auto body_size = writer.offset() - body_start;
    if (body_size > std::numeric_limits<std::uint16_t>::max()) {
        return failure(ControllerCodecStatus::FrameTooLarge, writer.offset());
    }
    const auto body_length = static_cast<std::uint16_t>(body_size);
    destination[4] = static_cast<std::uint8_t>(body_length & 0xFFU);
    destination[5] = static_cast<std::uint8_t>((body_length >> 8U) & 0xFFU);

    const auto checksum = crc32_iso_hdlc(destination, writer.offset());
    if (!writer.write_u32(checksum)) {
        const auto status = destination_capacity > kControllerCodecMaximumFrameSize
            ? ControllerCodecStatus::FrameTooLarge
            : writer.status();
        return failure(status, writer.offset());
    }
    return {ControllerCodecStatus::Success, writer.offset(), 0U};
}

ControllerCodecResult decode_controller_message(
    const std::uint8_t* frame,
    std::size_t frame_size,
    ControllerCodecDecodeStorage& storage,
    ControllerCodecDecodedMessage& decoded) noexcept {
    storage = {};
    decoded = {};
    if (frame == nullptr) {
        return failure(ControllerCodecStatus::NullBuffer);
    }
    if (frame_size < kHeaderSize + kCrcSize) {
        return failure(ControllerCodecStatus::InvalidFrameSize);
    }
    if (frame_size > kControllerCodecMaximumFrameSize) {
        return failure(ControllerCodecStatus::FrameTooLarge);
    }
    if (frame[0] != kMagicFirst || frame[1] != kMagicSecond) {
        return failure(ControllerCodecStatus::BadMagic);
    }
    if (frame[2] != kControllerCodecWireVersion) {
        return failure(ControllerCodecStatus::UnsupportedWireVersion, 2U);
    }

    ControllerMessageType message_type = ControllerMessageType::Unknown;
    if (!read_message_type(frame[3], message_type)) {
        return failure(ControllerCodecStatus::UnknownMessageType, 3U);
    }

    const auto body_length = static_cast<std::size_t>(
        static_cast<std::uint16_t>(frame[4]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(frame[5]) << 8U));
    const auto expected_size = kHeaderSize + body_length + kCrcSize;
    if (expected_size != frame_size) {
        return failure(ControllerCodecStatus::PayloadLengthMismatch, 4U);
    }

    const auto expected_crc = load_u32_le(frame + frame_size - kCrcSize);
    const auto actual_crc = crc32_iso_hdlc(frame, frame_size - kCrcSize);
    if (expected_crc != actual_crc) {
        return failure(ControllerCodecStatus::CrcMismatch, frame_size - kCrcSize);
    }

    Reader reader(frame + kHeaderSize, body_length);
    std::uint16_t major = 0U;
    std::uint16_t minor = 0U;
    std::uint64_t sequence_number = 0U;
    if (!reader.read_u16(major) || !reader.read_u16(minor) ||
        !reader.read_text(decoded.envelope.controller_id) ||
        !reader.read_text(decoded.envelope.controller_instance_id) ||
        !reader.read_u64(sequence_number) ||
        !reader.read_text(decoded.envelope.sent_at)) {
        const auto status = reader.status() == ControllerCodecStatus::Success
            ? ControllerCodecStatus::MalformedField
            : reader.status();
        return failure(status, kHeaderSize + reader.offset());
    }
    decoded.envelope.type = message_type;
    decoded.envelope.protocol_version = ControllerProtocolVersion{major, minor};
    decoded.envelope.sequence_number = sequence_number;

    auto payload_status = ControllerCodecStatus::Success;
    if (!read_payload(reader, message_type, storage, decoded.payload, payload_status)) {
        const auto status = payload_status != ControllerCodecStatus::Success
            ? payload_status
            : (reader.status() != ControllerCodecStatus::Success
                   ? reader.status()
                   : ControllerCodecStatus::MalformedField);
        return failure(status, kHeaderSize + reader.offset());
    }
    if (reader.offset() != reader.size()) {
        return failure(ControllerCodecStatus::PayloadLengthMismatch,
                       kHeaderSize + reader.offset());
    }
    if (validate_controller_message(decoded.envelope).status !=
        ControllerMessageValidationStatus::Valid) {
        return failure(ControllerCodecStatus::InvalidEnvelope,
                       kHeaderSize + reader.offset());
    }
    if (validate_controller_message_payload(message_type, decoded.payload).status !=
        ControllerPayloadValidationStatus::Valid) {
        return failure(ControllerCodecStatus::InvalidPayload,
                       kHeaderSize + reader.offset());
    }
    return {ControllerCodecStatus::Success, frame_size, 0U};
}

}  // namespace dockstation::hardware::interfaces
