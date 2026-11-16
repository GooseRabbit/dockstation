#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "hardware/interfaces/controller_instance_identity.hpp"

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

}  // namespace

int main() {
    static_assert(kControllerInstanceIdEntropySize == 16U);
    static_assert(kControllerInstanceIdTextSize == 36U);

    ControllerInstanceIdEntropyBytes bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(index);
    }

    ControllerInstanceIdText text{};
    format_controller_instance_id_uuid_v4(bytes, text);
    const std::string_view id{text.data(), text.size()};

    expect(id == "00010203-0405-4607-8809-0a0b0c0d0e0f",
           "formats a deterministic UUIDv4 golden vector");
    expect(id.size() == 36U,
           "produces exactly 36 characters without a trailing NUL");
    expect(id[8] == '-' && id[13] == '-' &&
               id[18] == '-' && id[23] == '-',
           "places UUID hyphens at the standard offsets");
    expect(id[14] == '4',
           "sets the RFC 9562 version nibble to 4");
    expect(id[19] == '8' || id[19] == '9' ||
               id[19] == 'a' || id[19] == 'b',
           "sets the RFC variant bits");
    expect(id.find_first_of("ABCDEF") == std::string_view::npos,
           "uses lowercase hexadecimal characters");

    ControllerInstanceIdEntropyBytes zero_bytes{};
    ControllerInstanceIdText zero_text{};
    format_controller_instance_id_uuid_v4(zero_bytes, zero_text);
    expect(std::string_view{zero_text.data(), zero_text.size()} ==
               "00000000-0000-4000-8000-000000000000",
           "formats the all-zero boundary input without invalid version/variant bits");

    ControllerInstanceIdEntropyBytes high_bytes{};
    high_bytes.fill(0xFFU);
    ControllerInstanceIdText high_text{};
    format_controller_instance_id_uuid_v4(high_bytes, high_text);
    expect(std::string_view{high_text.data(), high_text.size()} ==
               "ffffffff-ffff-4fff-bfff-ffffffffffff",
           "preserves random bits outside the UUID version and variant fields");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
