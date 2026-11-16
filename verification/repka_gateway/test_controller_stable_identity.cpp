#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "hardware/interfaces/controller_stable_identity.hpp"

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

void set_identifier(
    std::array<char, kControllerStableIdBufferSize>& target,
    std::size_t& size, std::string_view value) {
    target.fill('\0');
    size = value.size();
    for (std::size_t index = 0U;
         index < value.size() && index < target.size(); ++index) {
        target[index] = value[index];
    }
}
}  // namespace

int main() {
    constexpr std::string_view kRepkaId{"repka-controller-01"};
    constexpr std::string_view kEsp32Id{"esp32-controller-01"};

    expect(is_valid_controller_stable_id(kRepkaId),
           "accepts a valid Repka stable ID");
    expect(is_valid_controller_stable_id(kEsp32Id),
           "accepts a valid ESP32 stable ID");
    expect(!is_valid_controller_stable_id({}),
           "rejects an empty stable ID");
    expect(!is_valid_controller_stable_id("controller/id"),
           "rejects slash and path-like values");
    expect(!is_valid_controller_stable_id("controller id"),
           "rejects whitespace");
    expect(!is_valid_controller_stable_id("controller\n"),
           "rejects control characters");
    expect(!is_valid_controller_stable_id(
               std::string_view{"bad\0id", 6U}),
           "rejects an embedded NUL");
    expect(!is_valid_controller_stable_id("контроллер"),
           "rejects non-ASCII values consistently on both platforms");

    const std::string maximum_id(kControllerStableIdMaximumBytes, 'x');
    expect(is_valid_controller_stable_id(maximum_id),
           "accepts exactly 128 ASCII bytes");
    const std::string oversized_id(
        kControllerStableIdMaximumBytes + 1U, 'x');
    expect(!is_valid_controller_stable_id(oversized_id),
           "rejects 129-byte identifiers");

    ControllerStableIdPair pair{};
    expect(!pair.is_valid(),
           "default pair cannot be used as a configured identity");

    set_identifier(pair.local, pair.local_size, kRepkaId);
    set_identifier(pair.peer, pair.peer_size, kEsp32Id);
    expect(pair.is_valid(),
           "accepts two valid distinct endpoint IDs");
    expect(pair.local_id() == kRepkaId && pair.peer_id() == kEsp32Id,
           "returns the exact locally owned values");

    set_identifier(pair.peer, pair.peer_size, kRepkaId);
    expect(!pair.is_valid(),
           "rejects equal local and peer IDs");

    set_identifier(pair.peer, pair.peer_size, kEsp32Id);
    pair.local[pair.local_size] = 'x';
    expect(!pair.is_valid(),
           "rejects an identifier buffer without its expected terminator");

    pair.clear();
    expect(pair.local_size == 0U && pair.peer_size == 0U &&
               pair.local[0] == '\0' && pair.peer[0] == '\0' &&
               !pair.is_valid(),
           "clear removes both IDs and makes the pair unusable");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
