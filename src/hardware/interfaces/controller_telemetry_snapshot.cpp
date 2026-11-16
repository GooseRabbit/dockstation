#include "hardware/interfaces/controller_telemetry_snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace dockstation::hardware::interfaces {

bool ControllerTelemetrySnapshot::replace(
    const ControllerCodecDecodedMessage& message,
    std::uint64_t received_at_unix_ms) noexcept {
    if (message.envelope.type != ControllerMessageType::TelemetrySnapshot ||
        !message.envelope.sequence_number.has_value() ||
        *message.envelope.sequence_number == 0U ||
        validate_controller_message(message.envelope).status !=
            ControllerMessageValidationStatus::Valid ||
        validate_controller_message_payload(message.envelope.type,
                                             message.payload).status !=
            ControllerPayloadValidationStatus::Valid) {
        return false;
    }

    const auto* source =
        std::get_if<ControllerTelemetrySnapshotPayload>(&message.payload);
    if (source == nullptr ||
        source->measurement_count > kMaximumMeasurements) {
        return false;
    }

    // Build into a temporary fixed-capacity object. A failed copy never leaves
    // a partially updated snapshot visible to the API or Operator.
    ControllerTelemetrySnapshot next{};
    if (!next.append_text(message.envelope.controller_id, next.controller_id_) ||
        !next.append_text(message.envelope.controller_instance_id,
                          next.controller_instance_id_)) {
        return false;
    }

    next.sequence_number_ = *message.envelope.sequence_number;
    next.received_at_unix_ms_ = received_at_unix_ms;
    for (std::size_t index = 0U;
         index < source->measurement_count; ++index) {
        const auto& input = source->measurements[index];
        auto& output = next.measurements_[index];
        if (!next.append_text(input.sensor_id, output.sensor_id) ||
            !next.append_text(input.unit, output.unit) ||
            !next.append_text(input.measured_at, output.measured_at)) {
            return false;
        }
        output.quality = input.quality;

        if (const auto* value = std::get_if<bool>(&input.value)) {
            output.value_type = StoredValueType::Boolean;
            output.boolean_value = *value;
        } else if (const auto* value = std::get_if<std::int64_t>(&input.value)) {
            output.value_type = StoredValueType::SignedInteger;
            output.signed_value = *value;
        } else if (const auto* value = std::get_if<std::uint64_t>(&input.value)) {
            output.value_type = StoredValueType::UnsignedInteger;
            output.unsigned_value = *value;
        } else if (const auto* value = std::get_if<double>(&input.value)) {
            output.value_type = StoredValueType::FloatingPoint;
            output.floating_value = *value;
        } else if (const auto* value =
                       std::get_if<std::string_view>(&input.value)) {
            output.value_type = StoredValueType::Text;
            if (!next.append_text(*value, output.text_value)) {
                return false;
            }
        } else {
            return false;
        }
        ++next.measurement_count_;
    }

    next.available_ = true;
    *this = next;
    return true;
}

void ControllerTelemetrySnapshot::clear() noexcept {
    text_storage_.fill('\0');
    measurements_.fill({});
    controller_id_ = {};
    controller_instance_id_ = {};
    text_size_ = 0U;
    measurement_count_ = 0U;
    sequence_number_ = 0U;
    received_at_unix_ms_ = 0U;
    available_ = false;
}

bool ControllerTelemetrySnapshot::append_text(
    std::string_view value,
    TextReference& reference) noexcept {
    if (value.size() > text_storage_.size() - text_size_) {
        return false;
    }
    reference.offset = static_cast<std::uint16_t>(text_size_);
    reference.size = static_cast<std::uint16_t>(value.size());
    std::copy(value.begin(), value.end(),
              text_storage_.begin() + static_cast<std::ptrdiff_t>(text_size_));
    text_size_ += value.size();
    return true;
}

std::string_view ControllerTelemetrySnapshot::text(
    const TextReference& reference) const noexcept {
    const std::size_t offset = reference.offset;
    const std::size_t size = reference.size;
    if (offset > text_size_ || size > text_size_ - offset) {
        return {};
    }
    return {text_storage_.data() + offset, size};
}

std::string_view ControllerTelemetrySnapshot::controller_id() const noexcept {
    return available_ ? text(controller_id_) : std::string_view{};
}

std::string_view ControllerTelemetrySnapshot::controller_instance_id() const noexcept {
    return available_ ? text(controller_instance_id_) : std::string_view{};
}

bool ControllerTelemetrySnapshot::measurement(
    std::size_t index,
    ControllerTelemetryMeasurementView& output) const noexcept {
    if (!available_ || index >= measurement_count_) {
        return false;
    }
    const auto& stored = measurements_[index];
    output.sensor_id = text(stored.sensor_id);
    output.unit = text(stored.unit);
    output.measured_at = text(stored.measured_at);
    output.quality = stored.quality;
    switch (stored.value_type) {
        case StoredValueType::Boolean:
            output.value = stored.boolean_value;
            break;
        case StoredValueType::SignedInteger:
            output.value = stored.signed_value;
            break;
        case StoredValueType::UnsignedInteger:
            output.value = stored.unsigned_value;
            break;
        case StoredValueType::FloatingPoint:
            output.value = stored.floating_value;
            break;
        case StoredValueType::Text:
            output.value = text(stored.text_value);
            break;
        default:
            return false;
    }
    return true;
}

}  // namespace dockstation::hardware::interfaces
