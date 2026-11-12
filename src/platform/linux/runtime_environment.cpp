#include "platform/linux/runtime_environment.hpp"

#include <fstream>
#include <string_view>
#include <sys/utsname.h>
#include <utility>

namespace dockstation::platform::linux_runtime {
namespace {

constexpr std::string_view kOsReleasePath{"/etc/os-release"};
constexpr std::string_view kProcUptimePath{"/proc/uptime"};
constexpr std::string_view kProcMemoryPath{"/proc/meminfo"};
constexpr std::string_view kThermalSensorPath{
    "/sys/class/thermal/thermal_zone0/temp"};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string unquote_os_release_value(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') ||
            (first == '\'' && last == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return value;
}

RuntimeValue unavailable(std::string reason) {
    RuntimeValue result{};
    result.status = RuntimeValueStatus::Unavailable;
    result.reason = std::move(reason);
    return result;
}

RuntimeValue available(std::string value) {
    RuntimeValue result{};
    result.status = RuntimeValueStatus::Available;
    result.value = std::move(value);
    return result;
}

bool can_read_first_line(std::string_view path) {
    std::ifstream input{std::string{path}};
    if (!input.is_open()) {
        return false;
    }

    std::string line;
    return static_cast<bool>(std::getline(input, line));
}

bool read_uname(struct utsname& details) {
    return ::uname(&details) == 0;
}

}  // namespace

RuntimeValue read_os_version() {
    std::ifstream input{std::string{kOsReleasePath}};
    if (!input.is_open()) {
        return unavailable("Не удалось открыть /etc/os-release");
    }

    std::string line;
    std::string name;
    std::string version;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, separator);
        const std::string value = unquote_os_release_value(
            line.substr(separator + 1));

        if (key == "PRETTY_NAME" && !value.empty()) {
            return available(value);
        }
        if (key == "NAME") {
            name = value;
        } else if (key == "VERSION") {
            version = value;
        }
    }

    if (!name.empty() && !version.empty()) {
        return available(name + " " + version);
    }
    if (!name.empty()) {
        return available(name);
    }

    return unavailable("В /etc/os-release нет PRETTY_NAME или NAME");
}

RuntimeValue read_kernel_version() {
    struct utsname details {};
    if (!read_uname(details)) {
        return unavailable("Вызов uname() не выполнен");
    }
    if (details.release[0] == '\0') {
        return unavailable("uname() вернул пустую версию ядра");
    }
    return available(details.release);
}

RuntimeValue read_hardware_architecture() {
    struct utsname details {};
    if (!read_uname(details)) {
        return unavailable("Вызов uname() не выполнен");
    }
    if (details.machine[0] == '\0') {
        return unavailable("uname() вернул пустую архитектуру");
    }
    return available(details.machine);
}

RuntimeValue read_software_build_info() {
    std::string compiler;
#if defined(__clang__)
    compiler = std::string{"Clang "} + __clang_version__;
#elif defined(__GNUC__)
    compiler = std::string{"GCC "} + __VERSION__;
#else
    compiler = "неизвестный компилятор";
#endif

    std::string build_id = "не задан";
#ifdef DOCKSTATION_BUILD_ID
    build_id = DOCKSTATION_BUILD_ID;
#endif

    return available("build_id=" + build_id + "; compiler=" + compiler +
                     "; cxx_standard=17");
}

RuntimeDependencyReport check_runtime_dependencies() {
    struct utsname details {};
    RuntimeDependencyReport report{};
    report.uname_available = read_uname(details);
    report.proc_uptime_available = can_read_first_line(kProcUptimePath);
    report.proc_memory_available = can_read_first_line(kProcMemoryPath);
    report.os_release_available = can_read_first_line(kOsReleasePath);
    report.thermal_sensor_available = can_read_first_line(kThermalSensorPath);

    if (!report.uname_available) {
        report.status = RuntimeDependencyStatus::Unavailable;
    } else if (!report.proc_uptime_available || !report.proc_memory_available) {
        // Служба может работать, но часть системной диагностики будет недоступна.
        report.status = RuntimeDependencyStatus::Degraded;
    } else {
        // OS-release и thermal sysfs — дополнительные источники, а не условие запуска.
        report.status = RuntimeDependencyStatus::Ready;
    }
    return report;
}

}  // namespace dockstation::platform::linux_runtime
