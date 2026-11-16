#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dockstation::hardware::interfaces {

inline constexpr std::size_t kControllerInstanceIdEntropySize = 16U;
inline constexpr std::size_t kControllerInstanceIdTextSize = 36U;

using ControllerInstanceIdEntropyBytes =
    std::array<std::uint8_t, kControllerInstanceIdEntropySize>;
using ControllerInstanceIdText =
    std::array<char, kControllerInstanceIdTextSize>;

// Formats 16 bytes as an RFC 9562 UUIDv4 string (lowercase, 8-4-4-4-12).
// The input is copied before the version and variant bits are set.
void format_controller_instance_id_uuid_v4(
    const ControllerInstanceIdEntropyBytes& random_bytes,
    ControllerInstanceIdText& output) noexcept;

}  // namespace dockstation::hardware::interfaces
