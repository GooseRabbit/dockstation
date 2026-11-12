#pragma once

#include <cstdint>
#include <string>

namespace dockstation::platform::linux_diagnostics {

enum class DiagnosticValueStatus : std::uint8_t {
    Available,
    Unavailable
};

struct UptimeDiagnostic {
    DiagnosticValueStatus status{DiagnosticValueStatus::Unavailable};
    double seconds{0.0};
    std::string reason{};
};

struct MemoryDiagnostic {
    DiagnosticValueStatus status{DiagnosticValueStatus::Unavailable};
    std::uint64_t total_bytes{0};
    std::uint64_t available_bytes{0};
    std::uint64_t used_bytes{0};
    double used_percent{0.0};
    std::string reason{};
};

struct StorageDiagnostic {
    DiagnosticValueStatus status{DiagnosticValueStatus::Unavailable};
    std::string path{"/"};
    std::uint64_t total_bytes{0};
    std::uint64_t free_bytes{0};
    std::uint64_t available_bytes{0};
    std::uint64_t used_bytes{0};
    double used_percent{0.0};
    std::string reason{};
};

struct CpuTemperatureDiagnostic {
    DiagnosticValueStatus status{DiagnosticValueStatus::Unavailable};
    double celsius{0.0};
    std::string sensor_type{};
    std::string reason{};
};

// Linux load average is a count of runnable/queued tasks, not CPU utilization percent.
struct CpuLoadDiagnostic {
    DiagnosticValueStatus status{DiagnosticValueStatus::Unavailable};
    double one_minute{0.0};
    double five_minutes{0.0};
    double fifteen_minutes{0.0};
    std::uint64_t runnable_tasks{0};
    std::uint64_t total_tasks{0};
    std::string reason{};
};

struct SystemDiagnosticsSnapshot {
    UptimeDiagnostic uptime{};
    MemoryDiagnostic memory{};
    StorageDiagnostic storage{};
    CpuTemperatureDiagnostic cpu_temperature{};
    CpuLoadDiagnostic cpu_load{};
};

// Reads the first field of /proc/uptime in seconds since the last boot.
[[nodiscard]] UptimeDiagnostic read_uptime();

// Reads MemTotal and MemAvailable from /proc/meminfo; values are returned in bytes.
[[nodiscard]] MemoryDiagnostic read_memory_status();

// Reads capacity of the filesystem containing the root path ("/").
// A separately mounted optional SSD is not assumed by this function.
[[nodiscard]] StorageDiagnostic read_storage_status();

// Reads only a thermal-zone sensor explicitly labelled as CPU, SoC, or package.
// Unknown thermal zones are never presented as CPU temperature.
[[nodiscard]] CpuTemperatureDiagnostic read_cpu_temperature();

// Reads 1/5/15-minute Linux load averages and runnable/total task counts.
// These values are not CPU utilization percentages.
[[nodiscard]] CpuLoadDiagnostic read_cpu_load();

// Collects each diagnostic independently; one unavailable source does not erase others.
[[nodiscard]] SystemDiagnosticsSnapshot collect_system_diagnostics();

}  // namespace dockstation::platform::linux_diagnostics
