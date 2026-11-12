#include <cmath>
#include <cstdlib>
#include <iostream>

#include "platform/linux/system_diagnostics.hpp"

using namespace dockstation::platform::linux_diagnostics;

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

void expect_status_consistent(DiagnosticValueStatus status,
                              const std::string& reason,
                              const char* name) {
    expect(status == DiagnosticValueStatus::Available ||
               (status == DiagnosticValueStatus::Unavailable && !reason.empty()),
           name);
}

}  // namespace

int main() {
    const UptimeDiagnostic uptime = read_uptime();
    expect_status_consistent(uptime.status, uptime.reason,
                             "uptime status is explicit");
    if (uptime.status == DiagnosticValueStatus::Available) {
        expect(std::isfinite(uptime.seconds) && uptime.seconds >= 0.0,
               "uptime is a finite non-negative value");
    }

    const MemoryDiagnostic memory = read_memory_status();
    expect_status_consistent(memory.status, memory.reason,
                             "memory status is explicit");
    if (memory.status == DiagnosticValueStatus::Available) {
        expect(memory.total_bytes > 0, "memory total is positive");
        expect(memory.available_bytes <= memory.total_bytes,
               "available memory does not exceed total memory");
        expect(memory.used_bytes + memory.available_bytes == memory.total_bytes,
               "used and available memory sum to total");
        expect(std::isfinite(memory.used_percent) && memory.used_percent >= 0.0 &&
                   memory.used_percent <= 100.0,
               "memory usage percentage is in range");
    }

    const StorageDiagnostic storage = read_storage_status();
    expect_status_consistent(storage.status, storage.reason,
                             "root filesystem status is explicit");
    if (storage.status == DiagnosticValueStatus::Available) {
        expect(storage.path == "/", "storage result identifies the root filesystem");
        expect(storage.total_bytes > 0, "root filesystem total is positive");
        expect(storage.available_bytes <= storage.free_bytes,
               "unprivileged available space does not exceed free space");
        expect(storage.used_bytes + storage.free_bytes == storage.total_bytes,
               "used and free storage sum to total");
        expect(std::isfinite(storage.used_percent) && storage.used_percent >= 0.0 &&
                   storage.used_percent <= 100.0,
               "filesystem usage percentage is in range");
    }

    const CpuTemperatureDiagnostic temperature = read_cpu_temperature();
    expect_status_consistent(temperature.status, temperature.reason,
                             "CPU temperature status is explicit");
    if (temperature.status == DiagnosticValueStatus::Available) {
        expect(std::isfinite(temperature.celsius),
               "CPU temperature is finite");
        expect(!temperature.sensor_type.empty(),
               "CPU temperature identifies its sensor");
    }

    const CpuLoadDiagnostic load = read_cpu_load();
    expect_status_consistent(load.status, load.reason,
                             "CPU load status is explicit");
    if (load.status == DiagnosticValueStatus::Available) {
        expect(std::isfinite(load.one_minute) && load.one_minute >= 0.0,
               "one-minute load average is valid");
        expect(std::isfinite(load.five_minutes) && load.five_minutes >= 0.0,
               "five-minute load average is valid");
        expect(std::isfinite(load.fifteen_minutes) && load.fifteen_minutes >= 0.0,
               "fifteen-minute load average is valid");
        expect(load.total_tasks > 0 && load.runnable_tasks <= load.total_tasks,
               "runnable and total task counts are consistent");
    }

    const SystemDiagnosticsSnapshot snapshot = collect_system_diagnostics();
    expect(snapshot.uptime.status == uptime.status,
           "snapshot includes uptime result");
    expect(snapshot.memory.status == memory.status,
           "snapshot includes memory result");
    expect(snapshot.storage.status == storage.status,
           "snapshot includes storage result");
    expect(snapshot.cpu_temperature.status == temperature.status,
           "snapshot includes CPU temperature result");
    expect(snapshot.cpu_load.status == load.status,
           "snapshot includes CPU load result");

    std::cout << "Uptime seconds: " << uptime.seconds << '\n'
              << "Memory used bytes: " << memory.used_bytes << '\n'
              << "Root filesystem used bytes: " << storage.used_bytes << '\n'
              << "CPU temperature status: "
              << (temperature.status == DiagnosticValueStatus::Available
                      ? "AVAILABLE" : "UNAVAILABLE") << '\n'
              << "CPU load (1/5/15 min): " << load.one_minute << "/"
              << load.five_minutes << "/" << load.fifteen_minutes << '\n'
              << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
