#include "platform/linux/platform_capabilities.hpp"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <cstdlib>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace dockstation::platform::linux_runtime {
namespace {

constexpr std::size_t kPathBufferSize = 512U;
constexpr std::size_t kMaximumSeenDevices = 128U;

bool path_is_readable_directory(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    DIR* directory = ::opendir(path);
    if (directory == nullptr) {
        return false;
    }
    (void)::closedir(directory);
    return true;
}

template <std::size_t Size>
bool read_device_tree_property(
    const char* path, std::array<char, Size>& destination) noexcept {
    destination.fill('\0');
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::size_t used = 0U;
    char value = '\0';
    while (input.get(value)) {
        if (value == '\0') {
            if (used > 0U && destination[used - 1U] != ';' &&
                used + 1U < destination.size()) {
                destination[used++] = ';';
            }
            continue;
        }
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x20U || byte > 0x7EU) {
            continue;
        }
        if (used + 1U >= destination.size()) {
            break;
        }
        destination[used++] = value;
    }

    while (used > 0U &&
           (destination[used - 1U] == ';' || destination[used - 1U] == ' ')) {
        --used;
    }
    destination[used] = '\0';
    return used > 0U;
}

template <std::size_t Size>
bool read_primary_or_fallback_property(
    const char* primary, const char* fallback,
    std::array<char, Size>& destination) noexcept {
    if (read_device_tree_property(primary, destination)) {
        return true;
    }
    return read_device_tree_property(fallback, destination);
}

bool contains_case_insensitive(
    std::string_view text, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > text.size()) {
        return false;
    }
    for (std::size_t offset = 0U; offset + needle.size() <= text.size(); ++offset) {
        bool matches = true;
        for (std::size_t index = 0U; index < needle.size(); ++index) {
            const auto left = static_cast<unsigned char>(text[offset + index]);
            const auto right = static_cast<unsigned char>(needle[index]);
            if (std::tolower(left) != std::tolower(right)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

bool matches_device_name(
    std::string_view name, const char* const* prefixes,
    std::size_t prefix_count, bool match_all) noexcept {
    if (match_all) {
        return !name.empty() && name != "." && name != "..";
    }
    for (std::size_t index = 0U; index < prefix_count; ++index) {
        const std::string_view prefix{prefixes[index]};
        if (name.size() >= prefix.size() && name.substr(0U, prefix.size()) == prefix) {
            if (prefix == "serial0" && name != "serial0") {
                continue;
            }
            return true;
        }
    }
    return false;
}

bool is_kernel_console(
    const char* cmdline, std::string_view device_name) noexcept {
    if (cmdline == nullptr || device_name.empty()) {
        return false;
    }

    const char* cursor = cmdline;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') {
            ++cursor;
        }
        const char* token_begin = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\n') {
            ++cursor;
        }
        const std::string_view token{
            token_begin, static_cast<std::size_t>(cursor - token_begin)};
        constexpr std::string_view kConsolePrefix{"console="};
        if (token.size() <= kConsolePrefix.size() ||
            token.substr(0U, kConsolePrefix.size()) != kConsolePrefix) {
            continue;
        }
        const std::string_view console_value = token.substr(kConsolePrefix.size());
        const std::size_t comma = console_value.find(',');
        const std::string_view console_device = console_value.substr(0U, comma);
        if (console_device == device_name) {
            return true;
        }
    }
    return false;
}

bool read_kernel_cmdline(
    const char* path, std::array<char, 4096U>& destination) noexcept {
    destination.fill('\0');
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }
    input.getline(destination.data(),
                  static_cast<std::streamsize>(destination.size()));
    return !input.bad();
}

struct DeviceScanState final {
    PlatformInterfaceCapability& capability;
    const char* cmdline;
    dev_t seen_devices[kMaximumSeenDevices]{};
    std::size_t seen_count{0U};
};

void remember_device(
    DeviceScanState& state, const char* directory_path,
    const char* entry_name) noexcept {
    char full_path[kPathBufferSize]{};
    const int path_length = std::snprintf(
        full_path, sizeof(full_path), "%s/%s", directory_path, entry_name);
    if (path_length <= 0 ||
        static_cast<std::size_t>(path_length) >= sizeof(full_path)) {
        return;
    }

    struct stat metadata {};
    if (::stat(full_path, &metadata) != 0 || !S_ISCHR(metadata.st_mode)) {
        return;
    }

    for (std::size_t index = 0U; index < state.seen_count; ++index) {
        if (state.seen_devices[index] == metadata.st_rdev) {
            return;
        }
    }
    if (state.seen_count < kMaximumSeenDevices) {
        state.seen_devices[state.seen_count++] = metadata.st_rdev;
    }

    PlatformInterfaceCapability& capability = state.capability;
    if (capability.device_node_count < std::numeric_limits<std::uint32_t>::max()) {
        ++capability.device_node_count;
    }

    const bool accessible =
        ::faccessat(AT_FDCWD, full_path, R_OK | W_OK, AT_EACCESS) == 0;
    if (accessible) {
        if (capability.accessible_device_count <
            std::numeric_limits<std::uint32_t>::max()) {
            ++capability.accessible_device_count;
        }
        if (capability.accessible_device_count == 1U) {
            (void)std::snprintf(capability.first_device_path.data(),
                                capability.first_device_path.size(), "%s", full_path);
        }
    } else if (capability.first_device_path[0] == '\0') {
        (void)std::snprintf(capability.first_device_path.data(),
                            capability.first_device_path.size(), "%s", full_path);
    }

    bool console_conflict = is_kernel_console(state.cmdline, entry_name);
    if (!console_conflict) {
        // realpath() may produce a canonical path longer than our bounded
        // diagnostic path buffer. Passing nullptr asks libc to allocate the
        // exact required size and prevents a stack-buffer overwrite.
        char* resolved_path = ::realpath(full_path, nullptr);
        if (resolved_path != nullptr) {
            const char* resolved_name = std::strrchr(resolved_path, '/');
            resolved_name = resolved_name == nullptr ? resolved_path : resolved_name + 1;
            console_conflict = is_kernel_console(state.cmdline, resolved_name);
            std::free(resolved_path);
        }
    }
    if (console_conflict &&
        capability.kernel_console_conflict_count <
            std::numeric_limits<std::uint32_t>::max()) {
        ++capability.kernel_console_conflict_count;
    }
}

bool scan_device_directory(
    const char* directory_path, const char* const* prefixes,
    std::size_t prefix_count, bool match_all,
    DeviceScanState& state, bool missing_is_ok) noexcept {
    if (directory_path == nullptr || directory_path[0] == '\0') {
        return false;
    }
    DIR* directory = ::opendir(directory_path);
    if (directory == nullptr) {
        return missing_is_ok && errno == ENOENT;
    }

    dirent* entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string_view name{entry->d_name};
        if (!matches_device_name(name, prefixes, prefix_count, match_all)) {
            continue;
        }
        remember_device(state, directory_path, entry->d_name);
    }
    (void)::closedir(directory);
    return true;
}

bool matches_any_prefix(
    std::string_view name, const char* const* prefixes,
    std::size_t prefix_count) noexcept {
    for (std::size_t index = 0U; index < prefix_count; ++index) {
        const std::string_view prefix{prefixes[index]};
        if (name.size() >= prefix.size() && name.substr(0U, prefix.size()) == prefix) {
            return true;
        }
    }
    return false;
}

bool count_sysfs_entries(
    const char* sys_class_root, const char* class_name,
    const char* const* prefixes, std::size_t prefix_count,
    std::uint32_t& count) noexcept {
    char directory_path[kPathBufferSize]{};
    const int path_length = std::snprintf(
        directory_path, sizeof(directory_path), "%s/%s", sys_class_root,
        class_name);
    if (path_length <= 0 ||
        static_cast<std::size_t>(path_length) >= sizeof(directory_path)) {
        return false;
    }

    DIR* directory = ::opendir(directory_path);
    if (directory == nullptr) {
        return errno == ENOENT;
    }

    dirent* entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string_view name{entry->d_name};
        if (!matches_any_prefix(name, prefixes, prefix_count)) {
            continue;
        }
        char entry_path[kPathBufferSize]{};
        const int entry_length = std::snprintf(
            entry_path, sizeof(entry_path), "%s/%s", directory_path,
            entry->d_name);
        if (entry_length <= 0 ||
            static_cast<std::size_t>(entry_length) >= sizeof(entry_path)) {
            continue;
        }
        struct stat metadata {};
        if (::stat(entry_path, &metadata) == 0 && S_ISDIR(metadata.st_mode) &&
            count < std::numeric_limits<std::uint32_t>::max()) {
            ++count;
        }
    }
    (void)::closedir(directory);
    return true;
}

PlatformCapabilityStatus resolve_status(
    const PlatformInterfaceCapability& capability,
    bool device_source_readable, bool sysfs_source_readable) noexcept {
    if (capability.device_node_count > 0U) {
        return capability.accessible_device_count > 0U
            ? PlatformCapabilityStatus::Available
            : PlatformCapabilityStatus::Detected;
    }
    if (!device_source_readable || !sysfs_source_readable) {
        return PlatformCapabilityStatus::Unknown;
    }
    if (capability.sysfs_controller_count > 0U) {
        return PlatformCapabilityStatus::NotConfigured;
    }
    return PlatformCapabilityStatus::Unavailable;
}

PlatformInterfaceCapability probe_interface(
    const LinuxPlatformProbePaths& paths,
    const char* const* device_prefixes, std::size_t device_prefix_count,
    const char* sysfs_class_name,
    const char* const* sysfs_prefixes, std::size_t sysfs_prefix_count,
    const char* cmdline, bool scan_serial_aliases) noexcept {
    PlatformInterfaceCapability result{};
    DeviceScanState scan{result, cmdline};

    const bool device_source_readable = path_is_readable_directory(paths.device_root);
    bool device_scan_ok = false;
    if (device_source_readable) {
        device_scan_ok = scan_device_directory(
            paths.device_root, device_prefixes, device_prefix_count, false,
            scan, false);
        if (scan_serial_aliases) {
            char alias_path[kPathBufferSize]{};
            (void)std::snprintf(alias_path, sizeof(alias_path), "%s/serial/by-id",
                                paths.device_root);
            device_scan_ok = scan_device_directory(
                alias_path, device_prefixes, device_prefix_count, true, scan, true) &&
                device_scan_ok;
            (void)std::snprintf(alias_path, sizeof(alias_path), "%s/serial/by-path",
                                paths.device_root);
            device_scan_ok = scan_device_directory(
                alias_path, device_prefixes, device_prefix_count, true, scan, true) &&
                device_scan_ok;
        }
    }

    const bool sysfs_source_readable =
        path_is_readable_directory(paths.sys_class_root);
    bool sysfs_scan_ok = false;
    if (sysfs_source_readable) {
        sysfs_scan_ok = count_sysfs_entries(
            paths.sys_class_root, sysfs_class_name, sysfs_prefixes,
            sysfs_prefix_count, result.sysfs_controller_count);
    }

    result.status = resolve_status(
        result, device_source_readable && device_scan_ok,
        sysfs_source_readable && sysfs_scan_ok);
    return result;
}

}  // namespace

LinuxPlatformCapabilities detect_linux_platform_capabilities(
    const LinuxPlatformProbePaths& paths) noexcept {
    LinuxPlatformCapabilities result{};
    result.device_tree_model_available = read_primary_or_fallback_property(
        paths.device_tree_model, paths.fallback_device_tree_model,
        result.device_tree_model);
    result.device_tree_compatible_available = read_primary_or_fallback_property(
        paths.device_tree_compatible, paths.fallback_device_tree_compatible,
        result.device_tree_compatible);

    std::array<char, 4096U> cmdline{};
    result.kernel_cmdline_available =
        read_kernel_cmdline(paths.kernel_cmdline, cmdline);
    const char* cmdline_value = result.kernel_cmdline_available
        ? cmdline.data() : nullptr;

    const std::string_view model{result.device_tree_model.data()};
    const std::string_view compatible{result.device_tree_compatible.data()};
    const bool repka_pi5 =
        contains_case_insensitive(model, "Repka Pi 5") ||
        contains_case_insensitive(compatible, "repka,repka-pi5") ||
        contains_case_insensitive(compatible, "repka,pi5");

    const bool device_root_available = path_is_readable_directory(paths.device_root);
    const bool sys_class_available = path_is_readable_directory(paths.sys_class_root);
    if (repka_pi5) {
        result.platform_kind = LinuxPlatformKind::RepkaPi5;
    } else if (device_root_available && sys_class_available) {
        result.platform_kind = LinuxPlatformKind::GenericLinux;
    } else {
        result.platform_kind = LinuxPlatformKind::Unknown;
    }

    constexpr const char* uart_devices[] = {
        "ttyS", "ttyAMA", "ttyUSB", "ttyACM", "ttyFIQ", "serial0"};
    constexpr const char* uart_sysfs[] = {
        "ttyS", "ttyAMA", "ttyUSB", "ttyACM", "ttyFIQ"};
    result.uart = probe_interface(
        paths, uart_devices, std::size(uart_devices), "tty",
        uart_sysfs, std::size(uart_sysfs), cmdline_value, true);

    constexpr const char* gpio_devices[] = {"gpiochip", "repka_gpio"};
    constexpr const char* gpio_sysfs[] = {"gpiochip"};
    result.gpio = probe_interface(
        paths, gpio_devices, std::size(gpio_devices), "gpio",
        gpio_sysfs, std::size(gpio_sysfs), cmdline_value, false);

    constexpr const char* i2c_devices[] = {"i2c-"};
    constexpr const char* i2c_sysfs[] = {"i2c-"};
    result.i2c = probe_interface(
        paths, i2c_devices, std::size(i2c_devices), "i2c-adapter",
        i2c_sysfs, std::size(i2c_sysfs), cmdline_value, false);

    constexpr const char* spi_devices[] = {"spidev"};
    constexpr const char* spi_sysfs[] = {"spi"};
    result.spi = probe_interface(
        paths, spi_devices, std::size(spi_devices), "spi_master",
        spi_sysfs, std::size(spi_sysfs), cmdline_value, false);

    constexpr const char* pwm_devices[] = {"pwm"};
    constexpr const char* pwm_sysfs[] = {"pwmchip"};
    result.pwm = probe_interface(
        paths, pwm_devices, std::size(pwm_devices), "pwm",
        pwm_sysfs, std::size(pwm_sysfs), cmdline_value, false);

    constexpr const char* camera_devices[] = {"video", "media"};
    constexpr const char* camera_sysfs[] = {"video"};
    result.camera = probe_interface(
        paths, camera_devices, std::size(camera_devices), "video4linux",
        camera_sysfs, std::size(camera_sysfs), cmdline_value, false);

    return result;
}

const char* platform_kind_name(LinuxPlatformKind kind) noexcept {
    switch (kind) {
        case LinuxPlatformKind::GenericLinux: return "GENERIC_LINUX";
        case LinuxPlatformKind::RepkaPi5: return "REPKA_PI5";
        case LinuxPlatformKind::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* platform_capability_status_name(
    PlatformCapabilityStatus status) noexcept {
    switch (status) {
        case PlatformCapabilityStatus::Available: return "AVAILABLE";
        case PlatformCapabilityStatus::Detected: return "DETECTED";
        case PlatformCapabilityStatus::NotConfigured: return "NOT_CONFIGURED";
        case PlatformCapabilityStatus::Unavailable: return "UNAVAILABLE";
        case PlatformCapabilityStatus::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

}  // namespace dockstation::platform::linux_runtime
