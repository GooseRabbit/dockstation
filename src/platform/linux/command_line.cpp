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

    bool config_file_seen{false};
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
        std::uint64_t number{0U};

        if (option == "--config-file") {
            if (config_file_seen) return invalid("Параметр --config-file указан повторно");
            config_file_seen = true;
            if (!read_value(argc, argv, index, "--config-file", value, error)) {
                return invalid(error);
            }
            if (value.empty() || value.find('\0') != std::string_view::npos) {
                return invalid("Путь --config-file пуст или содержит нулевой байт");
            }
            result.config_file_path.assign(value.data(), value.size());
        } else if (option == "--api-port") {
            if (result.overrides.api_port) return invalid("Параметр --api-port указан повторно");
            result.overrides.api_port = true;
            if (!read_value(argc, argv, index, "--api-port", value, error) ||
                !parse_unsigned(value, number) || number == 0U || number > 65535U) {
                return invalid(error.empty()
                    ? "Некорректное значение --api-port (допустимо 1..65535)" : error);
            }
            result.config.api_port = static_cast<std::uint32_t>(number);
        } else if (option == "--log-level") {
            if (result.overrides.log_level) return invalid("Параметр --log-level указан повторно");
            result.overrides.log_level = true;
            if (!read_value(argc, argv, index, "--log-level", value, error) ||
                !parse_log_level(value, result.config.logger.minimum_level)) {
                return invalid(error.empty()
                    ? "Допустимые уровни --log-level: debug, info, warning, error" : error);
            }
        } else if (option == "--log-file") {
            if (result.overrides.log_file) return invalid("Параметр --log-file указан повторно");
            result.overrides.log_file = true;
            if (!read_value(argc, argv, index, "--log-file", value, error)) {
                return invalid(error);
            }
            if (value.empty()) return invalid("Путь --log-file не может быть пустым");
            result.config.logger.file_path.assign(value.data(), value.size());
        } else if (option == "--log-stderr" || option == "--no-log-stderr") {
            if (result.overrides.log_stderr) {
                return invalid("Параметр --log-stderr/--no-log-stderr указан повторно");
            }
            result.overrides.log_stderr = true;
            result.config.logger.write_to_stderr = option == "--log-stderr";
        } else if (option == "--log-max-file-size-bytes") {
            if (result.overrides.max_file_size_bytes) {
                return invalid("Параметр --log-max-file-size-bytes указан повторно");
            }
            result.overrides.max_file_size_bytes = true;
            if (!read_value(argc, argv, index, "--log-max-file-size-bytes", value, error) ||
                !parse_unsigned(value, number)) {
                return invalid(error.empty()
                    ? "Некорректное значение --log-max-file-size-bytes" : error);
            }
            result.config.logger.max_file_size_bytes = number;
        } else if (option == "--log-max-rotated-files") {
            if (result.overrides.max_rotated_files) {
                return invalid("Параметр --log-max-rotated-files указан повторно");
            }
            result.overrides.max_rotated_files = true;
            if (!read_value(argc, argv, index, "--log-max-rotated-files", value, error) ||
                !parse_unsigned(value, number) ||
                number > std::numeric_limits<std::size_t>::max() || number > 20U) {
                return invalid(error.empty()
                    ? "Некорректное значение --log-max-rotated-files (допустимо 0..20)" : error);
            }
            result.config.logger.max_rotated_files = static_cast<std::size_t>(number);
        } else {
            return invalid("Неизвестный параметр: " + std::string{option});
        }
    }

    if (result.config_file_path.empty()) {
        const RuntimeConfigValidationResult validation =
            validate_runtime_config(result.config);
        if (validation.status != RuntimeConfigValidationStatus::Valid) {
            return invalid("Некорректная настройка " + validation.field + ": " +
                           validation.reason);
        }
    }
    return result;
}

void apply_command_line_overrides(const CommandLineParseResult& parsed,
                                  RuntimeConfig& config) {
    if (parsed.overrides.api_port) config.api_port = parsed.config.api_port;
    if (parsed.overrides.log_level) {
        config.logger.minimum_level = parsed.config.logger.minimum_level;
    }
    if (parsed.overrides.log_file) config.logger.file_path = parsed.config.logger.file_path;
    if (parsed.overrides.log_stderr) {
        config.logger.write_to_stderr = parsed.config.logger.write_to_stderr;
    }
    if (parsed.overrides.max_file_size_bytes) {
        config.logger.max_file_size_bytes = parsed.config.logger.max_file_size_bytes;
    }
    if (parsed.overrides.max_rotated_files) {
        config.logger.max_rotated_files = parsed.config.logger.max_rotated_files;
    }
}

std::string command_line_help(const std::string& executable_name) {
    const std::string name = executable_name.empty() ? "dockstation-gateway" : executable_name;
    return "Использование: " + name + " [параметры]\n"
        "  --help                         Показать эту справку\n"
        "  --config-file PATH             Загрузить настройки из файла .conf\n"
        "  --api-port PORT                Порт локального API (по умолчанию 8080)\n"
        "  --log-level LEVEL              debug | info | warning | error\n"
        "  --log-file PATH                Дополнительный файл JSONL-журнала\n"
        "  --log-stderr                   Включить журналирование в stderr (по умолчанию)\n"
        "  --no-log-stderr                Отключить журналирование в stderr\n"
        "  --log-max-file-size-bytes N    Максимальный размер файла журнала (по умолчанию 1048576 байт)\n"
        "  --log-max-rotated-files N      Число архивов журнала (по умолчанию 3, не более 20)\n"
        "\n"
        "Адрес привязки API фиксирован: 127.0.0.1. Изменить его параметрами запуска нельзя.\n"
        "Приоритет настроек: значения по умолчанию, затем файл .conf, затем параметры запуска.\n";
}

}  // namespace dockstation::platform::linux_runtime
