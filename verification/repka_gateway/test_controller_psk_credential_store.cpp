#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "platform/linux/controller_psk_credential_store.hpp"

using namespace dockstation::platform::linux_platform;

namespace {

int failures = 0;
int checks = 0;

void expect(bool condition, const char* description) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << description << '\n';
    } else {
        std::cerr << "FAIL - " << description << '\n';
        ++failures;
    }
}

bool credentials_are_zero(const ControllerPskCredentials& credentials) noexcept {
    for (const std::uint8_t value : credentials.psk) {
        if (value != 0U) {
            return false;
        }
    }
    for (const std::uint8_t value : credentials.identity) {
        if (value != 0U) {
            return false;
        }
    }
    return true;
}

bool credentials_match_fixture(
    const ControllerPskCredentials& credentials) noexcept {
    for (std::size_t index = 0U; index < credentials.psk.size(); ++index) {
        if (credentials.psk[index] != static_cast<std::uint8_t>(index + 1U)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < credentials.identity.size(); ++index) {
        if (credentials.identity[index] !=
            static_cast<std::uint8_t>(kControllerPskSize + index + 1U)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || argv[1] == nullptr || argv[2] == nullptr) {
        std::cerr << "Usage: test_controller_psk_credential_store "
                     "<expect-success|expect-failure> <absolute-directory>\n";
        return EXIT_FAILURE;
    }

    const std::string_view expectation(argv[1]);
    ControllerPskCredentials credentials{};
    const ControllerPskCredentialFileStatus status =
        load_controller_psk_credentials_from_directory(argv[2], &credentials);

    if (expectation == "expect-success") {
        expect(status == ControllerPskCredentialFileStatus::Success,
               "accepts a correctly provisioned root-owned credential file");
        expect(credentials_match_fixture(credentials),
               "loads PSK and identity from their fixed binary offsets");
    } else if (expectation == "expect-failure") {
        expect(status != ControllerPskCredentialFileStatus::Success,
               "rejects a credential path that violates the storage contract");
        expect(credentials_are_zero(credentials),
               "clears destination credentials on every rejected load");
    } else {
        std::cerr << "Unknown test expectation\n";
        credentials.clear();
        return EXIT_FAILURE;
    }

    credentials.clear();
    expect(credentials_are_zero(credentials),
           "explicit credential clearing overwrites PSK and identity");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
