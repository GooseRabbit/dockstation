#include "platform/linux/system_diagnostics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <sys/statvfs.h>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <utility>

namespace dockstation::platform::linux_diagnostics {
namespace {

constexpr std::string_view kUptimePath{"/proc/uptime"};
constexpr std::string_view kMemoryPath{"/proc/meminfo"};
constexpr std::string_view kLoadAveragePath{"/proc/loadavg"};
constexpr std::string_view kThermalPath{"/sys/class/thermal"};
constexpr std::int64_t kMinimumPlausibleTemperatureMilliCelsius{-100000};
constexpr std::int64_t kMaximumPlausibleTemperatureMilliCelsius{250000};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_unsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_signed(std::string_view text, std::int64_t& value) {
    if (text.empty()) {
        return false;
    }
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_decimal(std::string_view text, double& value) {
    if (text.empty()) {
        return false;
    }
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value, std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == end && std::isfinite(value);
}

std::string first_token(std::string_view line, std::string_view& remainder) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        remainder = {};
        return {};
    }
    const auto end = line.find_first_of(" \t\r\n", first);
    if (end == std::string_view::npos) {
        remainder = {};
        return std::string{line.substr(first)};
    }
    const auto next = line.find_first_not_of(" \t\r\n", end);
    remainder = next == std::string_view::npos ? std::string_view{} : line.substr(next);
    return std::string{line.substr(first, end - first)};
}

template <typename Diagnostic>
void mark_unavailable(Diagnostic& diagnostic, std::string reason) {
    diagnostic.status = DiagnosticValueStatus::Unavailable;
    diagnostic.reason = std::move(reason);
}

template <typename Diagnostic>
void mark_available(Diagnostic& diagnostic) {
    diagnostic.status = DiagnosticValueStatus::Available;
    diagnostic.reason.clear();
}

bool parse_meminfo_kib(const std::string& line,
                       std::string_view requested_key,
                       std::uint64_t& value_kib) {
    const std::string_view view{line};
    const auto separator = view.find(':');
    if (separator == std::string_view::npos || view.substr(0, separator) != requested_key) {
        return false;
    }

    std::string_view remainder = view.substr(separator + 1);
    const auto first = remainder.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return false;
    }
    remainder.remove_prefix(first);
    const auto number_end = remainder.find_first_of(" \t");
    if (number_end == std::string_view::npos ||
        !parse_unsigned(remainder.substr(0, number_end), value_kib)) {
        return false;
    }
    remainder.remove_prefix(number_end);
    const auto unit_start = remainder.find_first_not_of(" \t");
    if (unit_start == std::string_view::npos) {
        return false;
    }
    remainder.remove_prefix(unit_start);
    const auto unit_end = remainder.find_first_of(" \t\r\n");
    const auto unit = remainder.substr(0, unit_end);
    return unit == "kB";
}

bool kib_to_bytes(std::uint64_t kib, std::uint64_t& bytes) {
    constexpr std::uint64_t kBytesPerKib{1024};
    if (kib > std::numeric_limits<std::uint64_t>::max() / kBytesPerKib) {
        return false;
    }
    bytes = kib * kBytesPerKib;
    return true;
}

bool blocks_to_bytes(std::uint64_t blocks,
                     std::uint64_t block_size,
                     std::uint64_t& bytes) {
    if (block_size != 0 &&
        blocks > std::numeric_limits<std::uint64_t>::max() / block_size) {
        return false;
    }
    bytes = blocks * block_size;
    return true;
}

bool read_first_line(std::string_view path, std::string& line) {
    std::ifstream input{std::string{path}};
    return input.is_open() && static_cast<bool>(std::getline(input, line));
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    });
    return value;
}

bool is_cpu_related_sensor(std::string_view type) {
    const std::string normalized = lowercase(std::string{type});
    return normalized.find("cpu") != std::string::npos ||
           normalized.find("soc") != std::string::npos ||
           normalized.find("package") != std::string::npos;
}

bool read_sensor_temperature(const std::string& directory,
                             std::string& sensor_type,
                             double& temperature_celsius,
                             std::string& reason) {
    const std::string type_path = directory + "/type";
    std::string type_line;
    if (!read_first_line(type_path, type_line)) {
        reason = "Не удалось прочитать тип термодатчика: " + type_path;
        return false;
    }
    sensor_type = trim(std::move(type_line));
    if (!is_cpu_related_sensor(sensor_type)) {
        return false;
    }

    const std::string temperature_path = directory + "/temp";
    std::string temperature_line;
    if (!read_first_line(temperature_path, temperature_line)) {
        reason = "Не удалось прочитать температуру датчика " + sensor_type;
        return false;
    }

    std::int64_t milli_celsius{0};
    if (!parse_signed(trim(std::move(temperature_line)), milli_celsius) ||
        milli_celsius < kMinimumPlausibleTemperatureMilliCelsius ||
        milli_celsius > kMaximumPlausibleTemperatureMilliCelsius) {
        reason = "Датчик " + sensor_type + " вернул некорректное значение температуры";
        return false;
    }

    temperature_celsius = static_cast<double>(milli_celsius) / 1000.0;
    return true;
}

}  // namespace

UptimeDiagnostic read_uptime() {
    UptimeDiagnostic result{};
    std::string line;
    if (!read_first_line(kUptimePath, line)) {
        mark_unavailable(result, "Не удалось прочитать /proc/uptime");
        return result;
    }

    std::string_view remainder;
    const std::string token = first_token(line, remainder);
    if (!parse_decimal(token, result.seconds) || result.seconds < 0.0) {
        mark_unavailable(result, "Некорректное значение uptime в /proc/uptime");
        result.seconds = 0.0;
        return result;
    }

    mark_available(result);
    return result;
}

MemoryDiagnostic read_memory_status() {
    MemoryDiagnostic result{};
    std::ifstream input{std::string{kMemoryPath}};
    if (!input.is_open()) {
        mark_unavailable(result, "Не удалось открыть /proc/meminfo");
        return result;
    }

    bool total_found{false};
    bool available_found{false};
    std::uint64_t total_kib{0};
    std::uint64_t available_kib{0};
    std::string line;
    while (std::getline(input, line)) {
        std::uint64_t parsed_value{0};
        if (!total_found && parse_meminfo_kib(line, "MemTotal", parsed_value)) {
            total_kib = parsed_value;
            total_found = true;
        } else if (!available_found &&
                   parse_meminfo_kib(line, "MemAvailable", parsed_value)) {
            available_kib = parsed_value;
            available_found = true;
        }
        if (total_found && available_found) {
            break;
        }
    }

    if (!total_found || !available_found || total_kib == 0 ||
        available_kib > total_kib || !kib_to_bytes(total_kib, result.total_bytes) ||
        !kib_to_bytes(available_kib, result.available_bytes)) {
        mark_unavailable(result,
                         "В /proc/meminfo отсутствуют корректные MemTotal/MemAvailable");
        result.total_bytes = 0;
        result.available_bytes = 0;
        return result;
    }

    result.used_bytes = result.total_bytes - result.available_bytes;
    result.used_percent =
        static_cast<double>(result.used_bytes) * 100.0 /
        static_cast<double>(result.total_bytes);
    mark_available(result);
    return result;
}

StorageDiagnostic read_storage_status() {
    StorageDiagnostic result{};
    struct statvfs details {};
    if (::statvfs(result.path.c_str(), &details) != 0) {
        mark_unavailable(result,
                         "Не удалось получить сведения о файловой системе /: " +
                             std::string{std::strerror(errno)});
        return result;
    }

    const std::uint64_t block_size = details.f_frsize != 0
        ? static_cast<std::uint64_t>(details.f_frsize)
        : static_cast<std::uint64_t>(details.f_bsize);
    const std::uint64_t total_blocks = static_cast<std::uint64_t>(details.f_blocks);
    const std::uint64_t free_blocks = static_cast<std::uint64_t>(details.f_bfree);
    const std::uint64_t available_blocks = static_cast<std::uint64_t>(details.f_bavail);

    if (block_size == 0 || free_blocks > total_blocks ||
        available_blocks > free_blocks ||
        !blocks_to_bytes(total_blocks, block_size, result.total_bytes) ||
        !blocks_to_bytes(free_blocks, block_size, result.free_bytes) ||
        !blocks_to_bytes(available_blocks, block_size, result.available_bytes) ||
        result.total_bytes == 0) {
        mark_unavailable(result, "Файловая система / вернула некорректные размеры блоков");
        result.total_bytes = 0;
        result.free_bytes = 0;
        result.available_bytes = 0;
        return result;
    }

    result.used_bytes = result.total_bytes - result.free_bytes;
    result.used_percent =
        static_cast<double>(result.used_bytes) * 100.0 /
        static_cast<double>(result.total_bytes);
    mark_available(result);
    return result;
}

CpuTemperatureDiagnostic read_cpu_temperature() {
    CpuTemperatureDiagnostic result{};
    DIR* directory = ::opendir(std::string{kThermalPath}.c_str());
    if (directory == nullptr) {
        mark_unavailable(result, "Не удалось открыть /sys/class/thermal");
        return result;
    }

    bool cpu_sensor_seen{false};
    std::string sensor_error;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const std::string entry_name{entry->d_name};
        if (entry_name.rfind("thermal_zone", 0) != 0) {
            continue;
        }

        std::string sensor_type;
        double temperature{0.0};
        std::string reason;
        const std::string sensor_directory =
            std::string{kThermalPath} + "/" + entry_name;
        if (read_sensor_temperature(sensor_directory, sensor_type, temperature, reason)) {
            result.sensor_type = std::move(sensor_type);
            result.celsius = temperature;
            ::closedir(directory);
            mark_available(result);
            return result;
        }
        if (is_cpu_related_sensor(sensor_type)) {
            cpu_sensor_seen = true;
            if (!reason.empty()) {
                sensor_error = std::move(reason);
            }
        }
    }
    ::closedir(directory);

    if (cpu_sensor_seen && !sensor_error.empty()) {
        mark_unavailable(result, std::move(sensor_error));
    } else {
        mark_unavailable(
            result,
            "В Linux thermal zones не найден датчик с явно CPU/SoC/package-меткой");
    }
    return result;
}

CpuLoadDiagnostic read_cpu_load() {
    CpuLoadDiagnostic result{};
    std::string line;
    if (!read_first_line(kLoadAveragePath, line)) {
        mark_unavailable(result, "Не удалось прочитать /proc/loadavg");
        return result;
    }

    std::string_view remainder{line};
    double one_minute{0.0};
    double five_minutes{0.0};
    double fifteen_minutes{0.0};
    double* const averages[] = {&one_minute, &five_minutes, &fifteen_minutes};
    for (double* average : averages) {
        const std::string token = first_token(remainder, remainder);
        if (!parse_decimal(token, *average) || *average < 0.0) {
            mark_unavailable(result, "Некорректные load average в /proc/loadavg");
            return result;
        }
    }

    const std::string task_counts = first_token(remainder, remainder);
    const auto separator = task_counts.find('/');
    std::uint64_t runnable_tasks{0};
    std::uint64_t total_tasks{0};
    if (separator == std::string::npos ||
        !parse_unsigned(std::string_view{task_counts}.substr(0, separator),
                        runnable_tasks) ||
        !parse_unsigned(std::string_view{task_counts}.substr(separator + 1),
                        total_tasks) ||
        runnable_tasks > total_tasks || total_tasks == 0) {
        mark_unavailable(result, "Некорректные счётчики задач в /proc/loadavg");
        return result;
    }

    result.one_minute = one_minute;
    result.five_minutes = five_minutes;
    result.fifteen_minutes = fifteen_minutes;
    result.runnable_tasks = runnable_tasks;
    result.total_tasks = total_tasks;
    mark_available(result);
    return result;
}

SystemDiagnosticsSnapshot collect_system_diagnostics() {
    SystemDiagnosticsSnapshot result{};
    result.uptime = read_uptime();
    result.memory = read_memory_status();
    result.storage = read_storage_status();
    result.cpu_temperature = read_cpu_temperature();
    result.cpu_load = read_cpu_load();
    return result;
}

}  // namespace dockstation::platform::linux_diagnostics
