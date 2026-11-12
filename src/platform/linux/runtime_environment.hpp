#pragma once

#include <cstdint>
#include <string>

namespace dockstation::platform::linux_runtime {

enum class RuntimeValueStatus : std::uint8_t {
    Available,
    Unavailable
};

struct RuntimeValue {
    RuntimeValueStatus status{RuntimeValueStatus::Unavailable};
    std::string value{};
    std::string reason{};
};

enum class RuntimeDependencyStatus : std::uint8_t {
    Ready,
    Degraded,
    Unavailable
};

// Ready: uname и оба обязательных источника procfs доступны.
// Degraded: uname доступен, но часть диагностики procfs недоступна.
// Unavailable: базовый системный вызов uname недоступен.
struct RuntimeDependencyReport {
    RuntimeDependencyStatus status{RuntimeDependencyStatus::Unavailable};
    bool uname_available{false};
    bool proc_uptime_available{false};
    bool proc_memory_available{false};
    bool os_release_available{false};
    bool thermal_sensor_available{false};
};

// Читает PRETTY_NAME из /etc/os-release. Файл не исполняется как shell-скрипт.
[[nodiscard]] RuntimeValue read_os_version();

// Получает версию ядра через uname(2).
[[nodiscard]] RuntimeValue read_kernel_version();

// Получает архитектуру через uname(2), например aarch64 или x86_64.
[[nodiscard]] RuntimeValue read_hardware_architecture();

// Возвращает сведения о сборке, не выдавая неизвестную версию за релиз.
[[nodiscard]] RuntimeValue read_software_build_info();

// Проверяет доступность системных источников диагностики. Не меняет настройки ОС.
[[nodiscard]] RuntimeDependencyReport check_runtime_dependencies();

}  // namespace dockstation::platform::linux_runtime
