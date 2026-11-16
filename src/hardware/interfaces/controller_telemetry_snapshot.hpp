#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hardware/interfaces/controller_codec.hpp"

namespace dockstation::hardware::interfaces {

struct ControllerTelemetryMeasurementView final {
    std::string_view sensor_id{};
    ControllerSensorValue value{};
    std::string_view unit{};
    SensorQualityStatus quality{SensorQualityStatus::Unknown};
    std::string_view measured_at{};
};

// Owns a bounded copy of one accepted peer snapshot. Decoded codec fields are
// string_views into a temporary frame, so the Gateway must not retain them.
class ControllerTelemetrySnapshot final {
public:
    static constexpr std::size_t kMaximumTextBytes =
        kControllerCodecMaximumFrameSize;
    static constexpr std::size_t kMaximumMeasurements =
        kControllerCodecMaximumMeasurements;

    [[nodiscard]] bool replace(
        const ControllerCodecDecodedMessage& message,
        std::uint64_t received_at_unix_ms) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] std::size_t measurement_count() const noexcept {
        return measurement_count_;
    }
    [[nodiscard]] std::uint64_t sequence_number() const noexcept {
        return sequence_number_;
    }
    [[nodiscard]] std::uint64_t received_at_unix_ms() const noexcept {
        return received_at_unix_ms_;
    }
    [[nodiscard]] std::string_view controller_id() const noexcept;
    [[nodiscard]] std::string_view controller_instance_id() const noexcept;
    [[nodiscard]] bool measurement(
        std::size_t index,
        ControllerTelemetryMeasurementView& output) const noexcept;

private:
    struct TextReference final {
        std::uint16_t offset{0U};
        std::uint16_t size{0U};
    };

    enum class StoredValueType : std::uint8_t {
        Boolean,
        SignedInteger,
        UnsignedInteger,
        FloatingPoint,
        Text
    };

    struct StoredMeasurement final {
        TextReference sensor_id{};
        TextReference unit{};
        TextReference measured_at{};
        TextReference text_value{};
        StoredValueType value_type{StoredValueType::Boolean};
        bool boolean_value{false};
        std::int64_t signed_value{0};
        std::uint64_t unsigned_value{0U};
        double floating_value{0.0};
        SensorQualityStatus quality{SensorQualityStatus::Unknown};
    };

    [[nodiscard]] bool append_text(
        std::string_view value,
        TextReference& reference) noexcept;
    [[nodiscard]] std::string_view text(
        const TextReference& reference) const noexcept;

    std::array<char, kMaximumTextBytes> text_storage_{};
    std::array<StoredMeasurement, kMaximumMeasurements> measurements_{};
    TextReference controller_id_{};
    TextReference controller_instance_id_{};
    std::size_t text_size_{0U};
    std::size_t measurement_count_{0U};
    std::uint64_t sequence_number_{0U};
    std::uint64_t received_at_unix_ms_{0U};
    bool available_{false};
};

}  // namespace dockstation::hardware::interfaces
