#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace dockstation::hardware::interfaces {

inline constexpr std::size_t kControllerStableIdMaximumBytes = 128U;
inline constexpr std::size_t kControllerStableIdBufferSize =
    kControllerStableIdMaximumBytes + 1U;

// Stable controller IDs are public protocol identifiers. They are not
// credentials, proofs of ownership or replacements for Station ID.
[[nodiscard]] inline bool is_valid_controller_stable_id(
    std::string_view value) noexcept {
    if (value.empty() || value.size() > kControllerStableIdMaximumBytes) {
        return false;
    }

    for (const unsigned char ch : value) {
        const bool alpha = (ch >= 'A' && ch <= 'Z') ||
                           (ch >= 'a' && ch <= 'z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '_' && ch != '-' &&
            ch != '.' && ch != ':') {
            return false;
        }
    }
    return true;
}

// Owns the local and peer stable IDs loaded from trusted configuration/NVS.
// The trailing NUL is stored but is not included in the exposed string views.
// This type neither generates nor persists identifiers.
struct ControllerStableIdPair final {
    std::array<char, kControllerStableIdBufferSize> local{};
    std::array<char, kControllerStableIdBufferSize> peer{};
    std::size_t local_size{0U};
    std::size_t peer_size{0U};

    void clear() noexcept {
        local.fill('\0');
        peer.fill('\0');
        local_size = 0U;
        peer_size = 0U;
    }

    [[nodiscard]] std::string_view local_id() const noexcept {
        return {local.data(), local_size};
    }

    [[nodiscard]] std::string_view peer_id() const noexcept {
        return {peer.data(), peer_size};
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return local_size > 0U &&
               local_size <= kControllerStableIdMaximumBytes &&
               peer_size > 0U &&
               peer_size <= kControllerStableIdMaximumBytes &&
               local[local_size] == '\0' &&
               peer[peer_size] == '\0' &&
               is_valid_controller_stable_id(local_id()) &&
               is_valid_controller_stable_id(peer_id()) &&
               local_id() != peer_id();
    }
};

}  // namespace dockstation::hardware::interfaces
