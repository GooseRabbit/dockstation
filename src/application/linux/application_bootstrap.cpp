#include "application/linux/application_bootstrap.hpp"

#include <csignal>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "platform/linux/command_line.hpp"
#include "platform/linux/network_diagnostics.hpp"
#include "platform/linux/runtime_config_file.hpp"
#include "platform/linux/runtime_environment.hpp"
#include "platform/linux/system_diagnostics.hpp"
#include "platform/logging/logger.hpp"

namespace dockstation::application::linux_app {
namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

extern "C" void handle_shutdown_signal(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        g_shutdown_requested = 1;
    }
}

class ShutdownSignalHandlers {
public:
    ShutdownSignalHandlers() = default;
    ShutdownSignalHandlers(const ShutdownSignalHandlers&) = delete;
    ShutdownSignalHandlers& operator=(const ShutdownSignalHandlers&) = delete;

    ~ShutdownSignalHandlers() {
        (void)restore();
    }

    bool install() noexcept {
        g_shutdown_requested = 0;

        struct sigaction action {};
        action.sa_handler = handle_shutdown_signal;
        action.sa_flags = 0;
        if (::sigemptyset(&action.sa_mask) != 0) {
            return false;
        }

        if (::sigaction(SIGINT, &action, &previous_sigint_) != 0) {
            return false;
        }
        sigint_installed_ = true;

        if (::sigaction(SIGTERM, &action, &previous_sigterm_) != 0) {
            (void)restore();
            return false;
        }
        sigterm_installed_ = true;
        return true;
    }

    bool restore() noexcept {
        bool restored = true;
        if (sigterm_installed_) {
            if (::sigaction(SIGTERM, &previous_sigterm_, nullptr) != 0) {
                restored = false;
            } else {
                sigterm_installed_ = false;
            }
        }
        if (sigint_installed_) {
            if (::sigaction(SIGINT, &previous_sigint_, nullptr) != 0) {
                restored = false;
            } else {
                sigint_installed_ = false;
            }
        }
        return restored;
    }

    [[nodiscard]] const volatile std::sig_atomic_t* flag() const noexcept {
        return &g_shutdown_requested;
    }

private:
    struct sigaction previous_sigint_ {};
    struct sigaction previous_sigterm_ {};
    bool sigint_installed_{false};
    bool sigterm_installed_{false};
};

bool write_log(platform::logging::LogLevel level,
               const std::string& message,
               const std::vector<platform::logging::LogField>& fields = {}) {
    const platform::logging::LoggerResult result =
        platform::logging::write_log_record(level, "application_bootstrap",
                                            message, fields);
    return result.status == platform::logging::LoggerStatus::Available;
}

bool log_runtime_value(const char* name,
                       const platform::linux_runtime::RuntimeValue& value) {
    using platform::logging::LogLevel;
    if (value.status == platform::linux_runtime::RuntimeValueStatus::Available) {
        return write_log(LogLevel::Info, "Runtime information available",
                         {{name, value.value}});
    }
    return write_log(LogLevel::Warning, "Runtime information unavailable",
                     {{name, value.reason}});
}

bool log_startup_diagnostics() {
    using platform::logging::LogLevel;
    using platform::linux_diagnostics::DiagnosticValueStatus;
    using platform::linux_diagnostics::NetworkDiagnosticStatus;

    const auto system = platform::linux_diagnostics::collect_system_diagnostics();
    bool logging_ok = true;

    const auto log_system_value = [&logging_ok](const char* name,
                                                DiagnosticValueStatus status,
                                                const std::string& value,
                                                const std::string& reason) {
        const bool written = status == DiagnosticValueStatus::Available
            ? write_log(LogLevel::Info, "System diagnostic available",
                        {{name, value}})
            : write_log(LogLevel::Warning, "System diagnostic unavailable",
                        {{name, reason}});
        logging_ok = written && logging_ok;
    };

    log_system_value("uptime_seconds", system.uptime.status,
                     std::to_string(system.uptime.seconds), system.uptime.reason);
    log_system_value("memory_total_bytes", system.memory.status,
                     std::to_string(system.memory.total_bytes) +
                         "; available_bytes=" +
                         std::to_string(system.memory.available_bytes) +
                         "; used_percent=" +
                         std::to_string(system.memory.used_percent),
                     system.memory.reason);
    log_system_value("root_storage_total_bytes", system.storage.status,
                     std::to_string(system.storage.total_bytes) +
                         "; available_bytes=" +
                         std::to_string(system.storage.available_bytes) +
                         "; used_percent=" +
                         std::to_string(system.storage.used_percent),
                     system.storage.reason);
    log_system_value("cpu_temperature_celsius", system.cpu_temperature.status,
                     std::to_string(system.cpu_temperature.celsius) +
                         "; sensor_type=" + system.cpu_temperature.sensor_type,
                     system.cpu_temperature.reason);
    log_system_value("cpu_load_average", system.cpu_load.status,
                     "1m=" + std::to_string(system.cpu_load.one_minute) +
                         "; 5m=" + std::to_string(system.cpu_load.five_minutes) +
                         "; 15m=" + std::to_string(system.cpu_load.fifteen_minutes) +
                         "; runnable=" +
                         std::to_string(system.cpu_load.runnable_tasks) +
                         "; total=" + std::to_string(system.cpu_load.total_tasks),
                     system.cpu_load.reason);

    const auto network = platform::linux_diagnostics::collect_network_diagnostics();
    const auto log_network_list = [&logging_ok](const char* name,
                                                NetworkDiagnosticStatus status,
                                                std::size_t count,
                                                const std::string& reason) {
        const bool written = status == NetworkDiagnosticStatus::Available
            ? write_log(LogLevel::Info, "Network diagnostic list available",
                        {{name, std::to_string(count)}})
            : write_log(LogLevel::Warning, "Network diagnostic list unavailable",
                        {{name, reason}});
        logging_ok = written && logging_ok;
    };

    log_network_list("network_interfaces", network.interfaces.status,
                     network.interfaces.values.size(), network.interfaces.reason);
    log_network_list("network_addresses", network.addresses.status,
                     network.addresses.values.size(), network.addresses.reason);
    log_network_list("default_routes", network.default_routes.status,
                     network.default_routes.values.size(),
                     network.default_routes.reason);

    for (const auto& interface : network.interfaces.values) {
        logging_ok = write_log(
            LogLevel::Info, "Network interface",
            {{"name", interface.name},
             {"index", std::to_string(interface.index)},
             {"up", interface.is_up ? "true" : "false"},
             {"running", interface.is_running ? "true" : "false"},
             {"loopback", interface.is_loopback ? "true" : "false"},
             {"mtu_available", interface.mtu_available ? "true" : "false"},
             {"mtu", interface.mtu_available ? std::to_string(interface.mtu) : ""}}) &&
            logging_ok;
    }
    for (const auto& address : network.addresses.values) {
        const char* family = address.family ==
                platform::linux_diagnostics::AddressFamily::IPv4
            ? "IPv4"
            : address.family == platform::linux_diagnostics::AddressFamily::IPv6
                ? "IPv6" : "UNSPECIFIED";
        logging_ok = write_log(
            LogLevel::Info, "Network interface address",
            {{"interface", address.interface_name},
             {"family", family},
             {"address", address.address},
             {"prefix_available",
              address.prefix_length_available ? "true" : "false"},
             {"prefix_length",
              address.prefix_length_available
                  ? std::to_string(address.prefix_length) : ""}}) &&
            logging_ok;
    }
    for (const auto& route : network.default_routes.values) {
        const char* family = route.family ==
                platform::linux_diagnostics::AddressFamily::IPv4
            ? "IPv4"
            : route.family == platform::linux_diagnostics::AddressFamily::IPv6
                ? "IPv6" : "UNSPECIFIED";
        logging_ok = write_log(
            LogLevel::Info, "Network default route",
            {{"family", family},
             {"interface", route.interface_name},
             {"gateway", route.gateway},
             {"metric", std::to_string(route.metric)}}) &&
            logging_ok;
    }
    return logging_ok;
}

}  // namespace

bool is_shutdown_requested(
    const volatile std::sig_atomic_t* shutdown_requested) noexcept {
    return shutdown_requested != nullptr && *shutdown_requested != 0;
}

ApplicationRunResult run_application(int argc,
                                     const char* const argv[],
                                     ServiceRunner service_runner) {
    using platform::logging::LogLevel;
    using platform::linux_runtime::CommandLineParseStatus;
    using platform::linux_runtime::parse_command_line;

    ApplicationRunResult result{};
    const auto parsed = parse_command_line(argc, argv);

    if (parsed.status == CommandLineParseStatus::HelpRequested) {
        const std::string executable =
            argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "";
        std::cout << platform::linux_runtime::command_line_help(executable);
        result.exit_code = ApplicationExitCode::Success;
        result.help_requested = true;
        result.message = "Справка выведена";
        return result;
    }
    if (parsed.status != CommandLineParseStatus::Success) {
        std::cerr << "Ошибка параметров запуска: " << parsed.error << '\n'
                  << "Используйте --help для просмотра справки.\n";
        result.exit_code = ApplicationExitCode::InvalidArguments;
        result.message = parsed.error;
        return result;
    }

    platform::linux_runtime::RuntimeConfig runtime_config{};
    if (!parsed.config_file_path.empty()) {
        const auto file_result =
            platform::linux_runtime::load_runtime_config_file(parsed.config_file_path);
        if (file_result.status !=
            platform::linux_runtime::RuntimeConfigFileStatus::Loaded) {
            std::cerr << "Не удалось загрузить конфигурацию: "
                      << file_result.field << ": " << file_result.reason;
            if (file_result.error_line != 0U) {
                std::cerr << " (строка " << file_result.error_line << ")";
            }
            std::cerr << '\n';
            result.exit_code = ApplicationExitCode::StartupFailure;
            result.message = file_result.reason;
            return result;
        }
        runtime_config = file_result.config;
    }
    platform::linux_runtime::apply_command_line_overrides(parsed, runtime_config);

    const auto config_validation =
        platform::linux_runtime::validate_runtime_config(runtime_config);
    if (config_validation.status !=
        platform::linux_runtime::RuntimeConfigValidationStatus::Valid) {
        std::cerr << "Некорректная итоговая конфигурация: "
                  << config_validation.field << ": " << config_validation.reason << '\n';
        result.exit_code = ApplicationExitCode::StartupFailure;
        result.message = config_validation.reason;
        return result;
    }

    const auto logger_result =
        platform::linux_runtime::initialize_runtime_logger(runtime_config);
    if (logger_result.status !=
        platform::linux_runtime::RuntimeLoggerInitializationStatus::Ready) {
        std::cerr << "Не удалось инициализировать журналирование: "
                  << logger_result.field << ": " << logger_result.reason << '\n';
        result.exit_code = ApplicationExitCode::StartupFailure;
        result.message = logger_result.reason;
        return result;
    }

    bool startup_ok = true;
    ApplicationExitCode exit_code = ApplicationExitCode::Success;
    ShutdownSignalHandlers signal_handlers;

    if (!signal_handlers.install()) {
        (void)write_log(LogLevel::Error, "Не удалось установить обработчики SIGINT/SIGTERM");
        startup_ok = false;
        exit_code = ApplicationExitCode::StartupFailure;
    }

    if (startup_ok &&
        !write_log(LogLevel::Info, "Запуск Linux-приложения DockStation (bootstrap)",
                   {{"configured_api_bind_address",
                     platform::linux_runtime::kGatewayBindAddress},
                    {"configured_api_port", std::to_string(runtime_config.api_port)}})) {
        startup_ok = false;
        exit_code = ApplicationExitCode::StartupFailure;
    }

    if (startup_ok) {
        const auto dependencies =
            platform::linux_runtime::check_runtime_dependencies();
        if (dependencies.status ==
            platform::linux_runtime::RuntimeDependencyStatus::Unavailable) {
            (void)write_log(LogLevel::Error,
                            "Критическая проверка системного окружения не пройдена",
                            {{"uname_available",
                              dependencies.uname_available ? "true" : "false"}});
            startup_ok = false;
            exit_code = ApplicationExitCode::StartupFailure;
        } else {
            const bool degraded =
                dependencies.status ==
                platform::linux_runtime::RuntimeDependencyStatus::Degraded;
            startup_ok = write_log(
                degraded ? LogLevel::Warning : LogLevel::Info,
                degraded ? "Системное окружение доступно частично"
                         : "Базовые системные зависимости доступны",
                {{"proc_uptime_available",
                  dependencies.proc_uptime_available ? "true" : "false"},
                 {"proc_memory_available",
                  dependencies.proc_memory_available ? "true" : "false"},
                 {"os_release_available",
                  dependencies.os_release_available ? "true" : "false"},
                 {"thermal_sensor_available",
                  dependencies.thermal_sensor_available ? "true" : "false"}});

            if (startup_ok) {
                startup_ok =
                    log_runtime_value("os_version",
                                  platform::linux_runtime::read_os_version()) &&
                    log_runtime_value("kernel_version",
                                  platform::linux_runtime::read_kernel_version()) &&
                    log_runtime_value("architecture",
                                  platform::linux_runtime::read_hardware_architecture()) &&
                    log_runtime_value("software_build",
                                  platform::linux_runtime::read_software_build_info());
            }
            if (startup_ok) {
                startup_ok = log_startup_diagnostics();
            }
            if (!startup_ok) {
                exit_code = ApplicationExitCode::StartupFailure;
            }
        }
    }

    if (startup_ok && is_shutdown_requested(signal_handlers.flag())) {
        (void)write_log(LogLevel::Warning,
                        "Получен сигнал завершения до запуска прикладной службы");
    } else if (startup_ok && service_runner != nullptr) {
        if (!write_log(LogLevel::Info, "Запуск зарегистрированной прикладной службы")) {
            startup_ok = false;
            exit_code = ApplicationExitCode::StartupFailure;
        } else {
            const int service_result =
                service_runner(runtime_config, signal_handlers.flag());
            if (service_result != 0) {
                (void)write_log(LogLevel::Error,
                                "Прикладная служба завершилась с ошибкой",
                                {{"service_exit_code",
                                  std::to_string(service_result)}});
                exit_code = ApplicationExitCode::ServiceFailure;
            } else {
                (void)write_log(LogLevel::Info,
                                "Прикладная служба завершилась штатно");
            }
        }
    } else if (startup_ok) {
        (void)write_log(
            LogLevel::Warning,
            "Bootstrap завершён; прикладные службы не зарегистрированы");
        std::cout << "Bootstrap DockStation инициализирован. "
                     "Прикладные службы пока не зарегистрированы; "
                     "HTTP API не запущен.\n";
    }

    if (!write_log(LogLevel::Info, "Завершение Linux-приложения DockStation")) {
        exit_code = ApplicationExitCode::ShutdownFailure;
    }

    if (!signal_handlers.restore()) {
        (void)write_log(LogLevel::Error,
                        "Не удалось восстановить исходные обработчики сигналов");
        exit_code = ApplicationExitCode::ShutdownFailure;
    }

    const auto flush_result = platform::logging::flush_log();
    if (flush_result.status != platform::logging::LoggerStatus::Available) {
        std::cerr << "Ошибка сброса журнала: " << flush_result.reason << '\n';
        exit_code = ApplicationExitCode::ShutdownFailure;
    }

    const auto shutdown_result = platform::logging::shutdown_logger();
    if (shutdown_result.status != platform::logging::LoggerStatus::Available) {
        std::cerr << "Ошибка завершения журналирования: "
                  << shutdown_result.reason << '\n';
        exit_code = ApplicationExitCode::ShutdownFailure;
    }

    result.exit_code = exit_code;
    if (exit_code == ApplicationExitCode::Success && service_runner == nullptr) {
        result.message =
            "Bootstrap завершён; прикладные службы не зарегистрированы; HTTP API не запущен";
    } else if (exit_code == ApplicationExitCode::Success) {
        result.message = "Bootstrap завершён";
    } else if (exit_code == ApplicationExitCode::ServiceFailure) {
        result.message = "Ошибка прикладной службы";
    } else if (exit_code == ApplicationExitCode::ShutdownFailure) {
        result.message = "Ошибка штатного завершения";
    } else {
        result.message = "Ошибка запуска";
    }
    return result;
}

}  // namespace dockstation::application::linux_app
