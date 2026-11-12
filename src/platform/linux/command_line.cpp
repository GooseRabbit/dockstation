#include "platform/linux/command_line.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace dockstation::platform::linux_runtime {
namespace {

CommandLineParseResult invalid(std::string reason) {
    CommandLineParseResult result{};
    result.status = CommandLineParseStatus::InvalidArguments;
    result.error = std::move(reason);
    return result;
}

bool parse_unsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool read_value(int argc,
                const char* const argv[],
                int& index,
                const char* option,
                std::string_view& value,
                std::string& error) {
    if (index + 1 >= argc || argv[index + 1] == nullptr) {
        error = std::string{"Для параметра "} + option + " не задано значение";
        return false;
    }
    const std::string_view candidate{argv[index + 1]};
    if (candidate.rfind("--", 0) == 0) {
        error = std::string{"Для параметра "} + option +
                " не задано значение перед следующим параметром";
        return false;
    }
    ++index;
    value = argv[index];
    return true;
}

bool parse_log_level(std::string_view value, logging::LogLevel& level) {
    if (value == "debug") {
        level = logging::LogLevel::Debug;
    } else if (value == "info") {
        level = logging::LogLevel::Info;
    } else if (value == "warning") {
        level = logging::LogLevel::Warning;
    } else if (value == "error") {
        level = logging::LogLevel::Error;
    } else {
        return false;
    }
    return true;
}

}  // namespace

CommandLineParseResult parse_command_line(int argc, const char* const argv[]) {
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
        return invalid("Некорректный массив аргументов запуска");
    }

    CommandLineParseResult result{};
    result.status = CommandLineParseStatus::Success;

    bool api_port_seen{false};
    bool log_level_seen{false};
    bool log_file_seen{false};
    bool log_stderr_seen{false};
    bool max_file_size_seen{false};
    bool max_rotated_files_seen{false};

    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return invalid("Обнаружен пустой указатель аргумента запуска");
        }
        const std::string_view option{argv[index]};
        if (option == "--help") {
            if (argc == 2) {
                result.status = CommandLineParseStatus::HelpRequested;
                return result;
            }
            return invalid("Параметр --help используется отдельно от остальных параметров");
        }

        std::string_view value;
        std::string error;
        std::uint64_t number{0};

        if (option == "--api-port") {
            if (api_port_seen) return invalid("Параметр --api-port указан повторно");
            api_port_seen = true;
            if (!read_value(argc, argv, index, "--api-port", value, error) ||
                !parse_unsigned(value, number) ||
                number > std::numeric_limits<std::uint32_t>::max()) {
                return invalid(error.empty() ? "Некорректное значение --api-port" : error);
            }
            result.config.api_port = static_cast<std::uint32_t>(number);
        } else if (option == "--log-level") {
            if (log_level_seen) return invalid("Параметр --log-level указан повторно");
            log_level_seen = true;
            if (!read_value(argc, argv, index, "--log-level", value, error) ||
                !parse_log_level(value, result.config.logger.minimum_level)) {
                return invalid(error.empty()
                    ? "Допустимые уровни --log-level: debug, info, warning, error"
                    : error);
            }
        } else if (option == "--log-file") {
            if (log_file_seen) return invalid("Параметр --log-file указан повторно");
            log_file_seen = true;
            if (!read_value(argc, argv, index, "--log-file", value, error)) {
                return invalid(error);
            }
            if (value.empty()) return invalid("Путь --log-file не может быть пустым");
            result.config.logger.file_path = std::string{value};
        } else if (option == "--log-stderr" || option == "--no-log-stderr") {
            if (log_stderr_seen) {
                return invalid("Параметр --log-stderr/--no-log-stderr указан повторно");
            }
            log_stderr_seen = true;
            result.config.logger.write_to_stderr = option == "--log-stderr";
        } else if (option == "--log-max-file-size-bytes") {
            if (max_file_size_seen) {
                return invalid("Параметр --log-max-file-size-bytes указан повторно");
            }
            max_file_size_seen = true;
            if (!read_value(argc, argv, index, "--log-max-file-size-bytes", value, error) ||
                !parse_unsigned(value, number)) {
                return invalid(error.empty()
                    ? "Некорректное значение --log-max-file-size-bytes"
                    : error);
            }
            result.config.logger.max_file_size_bytes = number;
        } else if (option == "--log-max-rotated-files") {
            if (max_rotated_files_seen) {
                return invalid("Параметр --log-max-rotated-files указан повторно");
            }
            max_rotated_files_seen = true;
            if (!read_value(argc, argv, index, "--log-max-rotated-files", value, error) ||
                !parse_unsigned(value, number) ||
                number > std::numeric_limits<std::size_t>::max()) {
                return invalid(error.empty()
                    ? "Некорректное значение --log-max-rotated-files"
                    : error);
            }
            result.config.logger.max_rotated_files = static_cast<std::size_t>(number);
        } else {
            return invalid("Неизвестный параметр: " + std::string{option});
        }
    }

    const RuntimeConfigValidationResult validation = validate_runtime_config(result.config);
    if (validation.status != RuntimeConfigValidationStatus::Valid) {
        return invalid("Некорректная настройка " + validation.field + ": " +
                       validation.reason);
    }
    return result;
}

std::string command_line_help(const std::string& executable_name) {
    const std::string name = executable_name.empty() ? "dockstation-gateway" : executable_name;
    return "Использование: " + name + " [параметры]\n"
        "  --help                         Показать эту справку\n"
        "  --api-port PORT                Порт локального API (по умолчанию 8080)\n"
        "  --log-level LEVEL              debug | info | warning | error\n"
        "  --log-file PATH                Дополнительный файл JSONL-журнала\n"
        "  --log-stderr                   Включить журналирование в stderr (по умолчанию)\n"
        "  --no-log-stderr                Отключить журналирование в stderr\n"
        "  --log-max-file-size-bytes N    Максимальный размер файла журнала (по умолчанию 1048576 байт)\n"
        "  --log-max-rotated-files N      Число архивов журнала (по умолчанию 3, не более 20)\n"
        "\n"
        "Адрес привязки API фиксирован: 127.0.0.1. Изменить его параметрами запуска нельзя.\n"
        "Файл конфигурации и переменные окружения на этом этапе не поддерживаются.\n";
}

}  // namespace dockstation::platform::linux_runtime
