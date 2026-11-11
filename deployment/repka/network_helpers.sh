# Verify that every supplied command is available. No commands are executed.
require_command() {
    if (( $# == 0 )); then
        printf '[FAIL] Зависимости: укажите хотя бы одну обязательную команду.\n'
        return 2
    fi

    local required_command
    local missing_count=0
    for required_command in "$@"; do
        if [[ -z "$required_command" ]]; then
            printf '[FAIL] Зависимости: некорректное имя команды: "%s".\n' "$required_command"
            ((missing_count += 1))
            continue
        fi
        if command -v "$required_command" >/dev/null 2>&1; then
            printf '[PASS] Зависимости: команда "%s" доступна.\n' "$required_command"
        else
            printf '[FAIL] Зависимости: обязательная команда "%s" не найдена в PATH.\n' "$required_command"
            ((missing_count += 1))
        fi
    done

    if ((missing_count > 0)); then
        return 2
    fi
    return 0
}

# Read os-release as data; never source/evaluate an operating-system file.
read_os_release() {
    local release_file="${1:-/etc/os-release}"
    local line
    local key
    local value
    local os_name=""
    local os_id=""
    local os_version=""

    if [[ ! -r "$release_file" ]]; then
        printf '[UNKNOWN] ОС: файл "%s" недоступен для чтения.\n' "$release_file"
        return 1
    fi

    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ -n "$line" && "$line" != \#* && "$line" == *=* ]] || continue
        key="${line%%=*}"
        value="${line#*=}"
        case "$key" in
            PRETTY_NAME|NAME|ID|VERSION_ID)
                value="${value%$'\r'}"
                if [[ "$value" == \"*\" ]]; then
                    value="${value#\"}"
                    value="${value%\"}"
                elif [[ "$value" == \'*\' ]]; then
                    value="${value#\'}"
                    value="${value%\'}"
                fi
                case "$key" in
                    PRETTY_NAME) os_name="$value" ;;
                    NAME) [[ -n "$os_name" ]] || os_name="$value" ;;
                    ID) os_id="$value" ;;
                    VERSION_ID) os_version="$value" ;;
                esac
                ;;
        esac
    done < "$release_file"

    if [[ -z "$os_name" ]]; then
        printf '[UNKNOWN] ОС: файл "%s" не содержит NAME или PRETTY_NAME.\n' "$release_file"
        return 1
    fi

    printf '[PASS] ОС: %s' "$os_name"
    [[ -n "$os_id" ]] && printf ' (ID=%s)' "$os_id"
    [[ -n "$os_version" ]] && printf ' (версия=%s)' "$os_version"
    printf '\n'
    return 0
}

# Read machine architecture and pointer width without assuming a specific CPU.
check_architecture() {
    local uname_command="${1:-uname}"
    local getconf_command="${2:-getconf}"
    local machine
    local word_size

    if ! command -v "$uname_command" >/dev/null 2>&1; then
        printf '[FAIL] Архитектура: команда "%s" не найдена.\n' "$uname_command"
        return 2
    fi
    if ! machine="$("$uname_command" -m 2>/dev/null)" || [[ ! "$machine" =~ ^[A-Za-z0-9_.-]+$ ]]; then
        printf '[UNKNOWN] Архитектура: не удалось достоверно определить uname -m.\n'
        return 1
    fi

    if ! command -v "$getconf_command" >/dev/null 2>&1; then
        printf '[WARN] Архитектура: CPU=%s; разрядность недоступна (нет %s).\n' "$machine" "$getconf_command"
        return 1
    fi
    if ! word_size="$("$getconf_command" LONG_BIT 2>/dev/null)" || [[ "$word_size" != "32" && "$word_size" != "64" ]]; then
        printf '[UNKNOWN] Архитектура: CPU=%s; неожиданное значение LONG_BIT="%s".\n' "$machine" "${word_size:-empty}"
        return 1
    fi

    printf '[PASS] Архитектура: CPU=%s, разрядность=%s бит.\n' "$machine" "$word_size"
    return 0
}

# Print the final severity accumulated by the read-only network diagnostics.
# The return code mirrors the supplied aggregate status and never hides a check result.
print_diagnostics_report() {
    local overall_status="${1:-}"

    if [[ "$#" -ne 1 ]]; then
        printf '[FAIL] Итоговый отчёт: ожидается ровно один аргумент — общий код диагностики.\n'
        return 2
    fi

    case "$overall_status" in
        0)
            printf '[PASS] Итоговая сетевая диагностика: все проверки завершились успешно.\n'
            return 0
            ;;
        1)
            printf '[WARN] Итоговая сетевая диагностика: есть предупреждения или недоступные для проверки состояния.\n'
            return 1
            ;;
        2)
            printf '[FAIL] Итоговая сетевая диагностика: обнаружен отказ высокого уровня или отсутствует обязательное средство проверки.\n'
            return 2
            ;;
        *)
            printf '[FAIL] Итоговый отчёт: некорректный общий код диагностики: "%s" (допустимы 0, 1 или 2).\n' "$overall_status"
            return 2
            ;;
    esac
}
