#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

#include "platform/linux/platform_capabilities.hpp"

using namespace dockstation::platform::linux_runtime;

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

bool make_directories(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error;
}

bool write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return output.good();
}

bool make_device_alias(const std::filesystem::path& path) {
    return ::symlink("/dev/null", path.c_str()) == 0;
}

struct Fixture final {
    std::filesystem::path root{};
    LinuxPlatformProbePaths paths{};
    std::array<std::string, 7U> path_storage{};

    bool initialize() {
        std::array<char, 64U> template_path{};
        constexpr char pattern[] = "/tmp/dockstation-platform-probe-XXXXXX";
        std::copy(std::begin(pattern), std::end(pattern), template_path.begin());
        char* created = ::mkdtemp(template_path.data());
        if (created == nullptr) {
            return false;
        }
        root = created;
        const auto dev = root / "dev";
        const auto sys_class = root / "sys/class";
        const auto dt = root / "sys/firmware/devicetree/base";
        const auto proc_dt = root / "proc/device-tree";
        const auto proc = root / "proc";
        if (!make_directories(dev) || !make_directories(sys_class) ||
            !make_directories(dt) || !make_directories(proc_dt) ||
            !make_directories(proc)) {
            return false;
        }

        path_storage[0] = (root / "dev").string();
        path_storage[1] = (root / "sys/class").string();
        path_storage[2] = (root / "sys/firmware/devicetree/base/model").string();
        path_storage[3] = (root / "sys/firmware/devicetree/base/compatible").string();
        path_storage[4] = (root / "proc/device-tree/model").string();
        path_storage[5] = (root / "proc/device-tree/compatible").string();
        path_storage[6] = (root / "proc/cmdline").string();
        paths.device_root = path_storage[0].c_str();
        paths.sys_class_root = path_storage[1].c_str();
        paths.device_tree_model = path_storage[2].c_str();
        paths.device_tree_compatible = path_storage[3].c_str();
        paths.fallback_device_tree_model = path_storage[4].c_str();
        paths.fallback_device_tree_compatible = path_storage[5].c_str();
        paths.kernel_cmdline = path_storage[6].c_str();
        return true;
    }

    void cleanup() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

}  // namespace

int main() {
    Fixture fixture{};
    expect(fixture.initialize(), "fixture root and probe paths initialized");
    if (fixture.root.empty()) {
        return 1;
    }

    const auto dt_model = fixture.root / "proc/device-tree/model";
    const auto dt_compatible = fixture.root / "proc/device-tree/compatible";
    std::string model{"Repka Pi 5"};
    model.push_back('\0');
    std::string compatible{"repka,repka-pi5"};
    compatible.push_back('\0');
    compatible += "rockchip,rk3588";
    compatible.push_back('\0');
    expect(write_text(dt_model, model), "fixture Device Tree model written");
    expect(write_text(dt_compatible, compatible),
           "fixture Device Tree compatible written");
    expect(write_text(fixture.root / "proc/cmdline",
                      "root=/dev/mmcblk1 console=ttyS0,1500000n8 quiet\n"),
           "fixture kernel command line written");

    expect(make_directories(fixture.root / "sys/class/tty/ttyS0"),
           "UART sysfs fixture created");
    expect(make_directories(fixture.root / "sys/class/gpio/gpiochip0"),
           "GPIO sysfs fixture created");
    expect(make_directories(fixture.root / "sys/class/i2c-adapter/i2c-1"),
           "I2C sysfs fixture created");
    expect(make_directories(fixture.root / "sys/class/spi_master"),
           "SPI sysfs class created without a controller");
    expect(make_directories(fixture.root / "sys/class/pwm/pwmchip0"),
           "PWM sysfs fixture created");
    expect(make_directories(fixture.root / "sys/class/video4linux/video0"),
           "camera sysfs fixture created");

    expect(make_device_alias(fixture.root / "dev/ttyS0"),
           "UART fixture character-device alias created");
    expect(make_device_alias(fixture.root / "dev/gpiochip0"),
           "GPIO fixture character-device alias created");
    expect(make_device_alias(fixture.root / "dev/video0"),
           "camera fixture character-device alias created");

    const LinuxPlatformCapabilities result =
        detect_linux_platform_capabilities(fixture.paths);
    expect(result.platform_kind == LinuxPlatformKind::RepkaPi5,
           "Repka Pi 5 detected from Device Tree fallback properties");
    expect(result.device_tree_model_available &&
               std::string(result.device_tree_model.data()) == "Repka Pi 5",
           "Device Tree model is normalized and retained");
    expect(result.device_tree_compatible_available &&
               std::string(result.device_tree_compatible.data()).find(
                   "rockchip,rk3588") != std::string::npos,
           "NUL-separated compatible values are normalized");
    expect(result.kernel_cmdline_available,
           "kernel command line availability is reported");

    expect(result.uart.status == PlatformCapabilityStatus::Available,
           "UART is available when a device node is accessible");
    expect(result.uart.kernel_console_conflict_count == 1U,
           "UART used as kernel console is reported as a conflict");
    expect(result.uart.first_device_path[0] != '\0',
           "UART candidate path is returned for diagnostics only");
    expect(result.gpio.status == PlatformCapabilityStatus::Available,
           "GPIO character device availability is detected");
    expect(result.gpio.sysfs_controller_count == 1U,
           "GPIO sysfs controller count is reported");
    expect(result.i2c.status == PlatformCapabilityStatus::NotConfigured,
           "I2C controller without a userspace device node is not configured");
    expect(result.spi.status == PlatformCapabilityStatus::Unavailable,
           "SPI is unavailable when both device and sysfs sources are empty");
    expect(result.pwm.status == PlatformCapabilityStatus::NotConfigured,
           "PWM controller without exported userspace device is not configured");
    expect(result.camera.status == PlatformCapabilityStatus::Available,
           "V4L2 camera device availability is detected");

    Fixture generic_fixture{};
    expect(generic_fixture.initialize(), "generic Linux fixture initialized");
    if (!generic_fixture.root.empty()) {
        std::string generic_model{"Virtual Linux board"};
        generic_model.push_back('\0');
        expect(write_text(generic_fixture.root / "proc/device-tree/model",
                          generic_model),
               "generic fixture model written");
        const auto generic = detect_linux_platform_capabilities(generic_fixture.paths);
        expect(generic.platform_kind == LinuxPlatformKind::GenericLinux,
               "unrecognized board selects generic Linux profile");
        generic_fixture.cleanup();
    }

    const std::string missing_dev = (fixture.root / "missing-dev").string();
    const std::string missing_sys_class = (fixture.root / "missing-sys-class").string();
    LinuxPlatformProbePaths unavailable_paths{};
    unavailable_paths.device_root = missing_dev.c_str();
    unavailable_paths.sys_class_root = missing_sys_class.c_str();
    unavailable_paths.device_tree_model = "/missing/model";
    unavailable_paths.device_tree_compatible = "/missing/compatible";
    unavailable_paths.fallback_device_tree_model = "/missing/model-fallback";
    unavailable_paths.fallback_device_tree_compatible = "/missing/compatible-fallback";
    unavailable_paths.kernel_cmdline = "/missing/cmdline";
    const auto unknown = detect_linux_platform_capabilities(unavailable_paths);
    expect(unknown.platform_kind == LinuxPlatformKind::Unknown,
           "missing OS device sources leave platform identity unknown");
    fixture.cleanup();

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
