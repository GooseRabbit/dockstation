#pragma once

#include <array>
#include <cstdint>

namespace dockstation::platform::linux_runtime {

enum class LinuxPlatformKind : std::uint8_t {
    GenericLinux,
    RepkaPi5,
    Unknown
};

enum class PlatformCapabilityStatus : std::uint8_t {
    Available,
    Detected,
    NotConfigured,
    Unavailable,
    Unknown
};

struct PlatformInterfaceCapability final {
    PlatformCapabilityStatus status{PlatformCapabilityStatus::Unknown};
    std::uint32_t device_node_count{0U};
    std::uint32_t accessible_device_count{0U};
    std::uint32_t sysfs_controller_count{0U};
    std::uint32_t kernel_console_conflict_count{0U};
    // Diagnostic candidate only. It is never selected automatically for I/O.
    std::array<char, 128U> first_device_path{};
};

struct LinuxPlatformCapabilities final {
    LinuxPlatformKind platform_kind{LinuxPlatformKind::Unknown};
    bool device_tree_model_available{false};
    bool device_tree_compatible_available{false};
    bool kernel_cmdline_available{false};
    std::array<char, 128U> device_tree_model{};
    std::array<char, 256U> device_tree_compatible{};
    PlatformInterfaceCapability uart{};
    PlatformInterfaceCapability gpio{};
    PlatformInterfaceCapability i2c{};
    PlatformInterfaceCapability spi{};
    PlatformInterfaceCapability pwm{};
    PlatformInterfaceCapability camera{};
};

// Explicit paths make the read-only probe testable against a fixture tree.
// Production defaults inspect the current Linux system.
struct LinuxPlatformProbePaths final {
    const char* device_root{"/dev"};
    const char* sys_class_root{"/sys/class"};
    const char* device_tree_model{"/sys/firmware/devicetree/base/model"};
    const char* device_tree_compatible{
        "/sys/firmware/devicetree/base/compatible"};
    const char* fallback_device_tree_model{"/proc/device-tree/model"};
    const char* fallback_device_tree_compatible{
        "/proc/device-tree/compatible"};
    const char* kernel_cmdline{"/proc/cmdline"};
};

// Performs passive discovery only: no device is opened, configured, written,
// exported, reset, or selected for application use.
[[nodiscard]] LinuxPlatformCapabilities detect_linux_platform_capabilities(
    const LinuxPlatformProbePaths& paths = {}) noexcept;

[[nodiscard]] const char* platform_kind_name(LinuxPlatformKind kind) noexcept;
[[nodiscard]] const char* platform_capability_status_name(
    PlatformCapabilityStatus status) noexcept;

}  // namespace dockstation::platform::linux_runtime
