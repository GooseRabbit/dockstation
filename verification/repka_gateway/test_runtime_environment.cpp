#include <cstdlib>
#include <iostream>

#include "platform/linux/runtime_environment.hpp"

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

void expect_available(const RuntimeValue& value, const char* name) {
    expect(value.status == RuntimeValueStatus::Available, name);
    expect(!value.value.empty(), "available runtime value is not empty");
    expect(value.reason.empty(), "available runtime value has no failure reason");
}
}  // namespace

int main() {
    const RuntimeValue os = read_os_version();
    const RuntimeValue kernel = read_kernel_version();
    const RuntimeValue architecture = read_hardware_architecture();
    const RuntimeValue build = read_software_build_info();
    const RuntimeDependencyReport dependencies = check_runtime_dependencies();

    expect_available(os, "OS version is available");
    expect_available(kernel, "kernel version is available");
    expect_available(architecture, "hardware architecture is available");
    expect_available(build, "software build information is available");
    expect(dependencies.uname_available, "uname dependency is available");
    expect(dependencies.proc_uptime_available, "/proc/uptime is readable");
    expect(dependencies.proc_memory_available, "/proc/meminfo is readable");
    const RuntimeDependencyStatus expected_status =
        !dependencies.uname_available
            ? RuntimeDependencyStatus::Unavailable
            : (!dependencies.proc_uptime_available || !dependencies.proc_memory_available
                   ? RuntimeDependencyStatus::Degraded
                   : RuntimeDependencyStatus::Ready);
    expect(dependencies.status == expected_status,
           "dependency status reflects required sources only");

    std::cout << "OS: " << os.value << '\n'
              << "Kernel: " << kernel.value << '\n'
              << "Architecture: " << architecture.value << '\n'
              << "Build: " << build.value << '\n'
              << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
