#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/linux/controller_psk_tls_client.hpp"

namespace dockstation::platform::linux_platform {

inline constexpr char kControllerCredentialDirectory[] =
    "/etc/dockstation/credentials";
inline constexpr char kControllerCredentialFileName[] = "repka-esp32.psk";
inline constexpr std::size_t kControllerCredentialFileSize =
    kControllerPskSize + kControllerPskIdentitySize;

enum class ControllerPskCredentialFileStatus : std::uint8_t {
    Success,
    InvalidArgument,
    DirectoryUnavailable,
    DirectoryPermissionsInvalid,
    CredentialFileUnavailable,
    CredentialFilePermissionsInvalid,
    CredentialFileFormatInvalid,
    CredentialFileReadFailed
};

// Loads the provisioned binary file from a root-owned directory. The default
// location is /etc/dockstation/credentials/repka-esp32.psk. The output object
// is cleared before the operation and remains all-zero on every failure.
[[nodiscard]] ControllerPskCredentialFileStatus
load_controller_psk_credentials(
    ControllerPskCredentials* credentials) noexcept;

// Same security policy as the default loader, but with a caller-supplied
// absolute directory. This narrow seam is used by privileged host acceptance
// tests; the credential filename and owner/group/mode policy stay fixed.
[[nodiscard]] ControllerPskCredentialFileStatus
load_controller_psk_credentials_from_directory(
    const char* absolute_directory,
    ControllerPskCredentials* credentials) noexcept;

}  // namespace dockstation::platform::linux_platform
