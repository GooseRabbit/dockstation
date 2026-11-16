#include "hardware/interfaces/controller_instance_identity.hpp"

namespace dockstation::hardware::interfaces {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

void format_controller_instance_id_uuid_v4(
    const ControllerInstanceIdEntropyBytes& random_bytes,
    ControllerInstanceIdText& output) noexcept {
    auto uuid_bytes = random_bytes;

    // RFC 9562 UUIDv4: four version bits and the RFC variant bits are fixed;
    // the remaining 122 bits retain their hardware-RNG input.
    uuid_bytes[6] = static_cast<std::uint8_t>(
        (uuid_bytes[6] & 0x0FU) | 0x40U);
    uuid_bytes[8] = static_cast<std::uint8_t>(
        (uuid_bytes[8] & 0x3FU) | 0x80U);

    std::size_t output_offset = 0U;
    for (std::size_t index = 0U; index < uuid_bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            output[output_offset++] = '-';
        }

        const std::uint8_t value = uuid_bytes[index];
        output[output_offset++] = kHexDigits[(value >> 4U) & 0x0FU];
        output[output_offset++] = kHexDigits[value & 0x0FU];
    }
}

}  // namespace dockstation::hardware::interfaces
