#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/network_helpers.sh"

# Read-only preflight for the exact Repka OS image used by DockStation.
# This script never installs packages, changes boot settings, touches pinmux,
# changes device permissions, or starts/stops system services.
PLATFORM_ROOT="${DOCKSTATION_PLATFORM_ROOT:-}"
UNAME_COMMAND="${DOCKSTATION_UNAME_COMMAND:-uname}"
GETCONF_COMMAND="${DOCKSTATION_GETCONF_COMMAND:-getconf}"
CMAKE_COMMAND="${DOCKSTATION_CMAKE_COMMAND:-cmake}"
CXX_COMMAND="${DOCKSTATION_CXX_COMMAND:-${CXX:-c++}}"
PKG_CONFIG_COMMAND="${DOCKSTATION_PKG_CONFIG_COMMAND:-pkg-config}"
SYSTEMCTL_COMMAND="${DOCKSTATION_SYSTEMCTL_COMMAND:-systemctl}"
SYSTEMD_ANALYZE_COMMAND="${DOCKSTATION_SYSTEMD_ANALYZE_COMMAND:-systemd-analyze}"
SYSTEMD_UNIT_DIR="${DOCKSTATION_SYSTEMD_UNIT_DIR:-}"
OVERALL_STATUS=0

platform_path() {
    local absolute_path="$1"
    if [[ -n "$PLATFORM_ROOT" ]]; then
        printf '%s%s\n' "${PLATFORM_ROOT%/}" "$absolute_path"
    else
        printf '%s\n' "$absolute_path"
    fi
}

report() {
    local severity="$1"
    shift
    printf '[%s] %s\n' "$severity" "$*"
    case "$severity" in
        WARN)
            ((OVERALL_STATUS < 1)) && OVERALL_STATUS=1
            ;;
        FAIL)
            OVERALL_STATUS=2
            ;;
    esac
}

version_at_least() {
    local actual="${1%%[-+]*}"
    local required="${2%%[-+]*}"
    local actual_major actual_minor actual_patch
    local required_major required_minor required_patch

    IFS=. read -r actual_major actual_minor actual_patch <<< "$actual"
    IFS=. read -r required_major required_minor required_patch <<< "$required"
    actual_patch="${actual_patch:-0}"
    required_patch="${required_patch:-0}"

    [[ "$actual_major" =~ ^[0-9]+$ && "$actual_minor" =~ ^[0-9]+$ &&
       "$actual_patch" =~ ^[0-9]+$ && "$required_major" =~ ^[0-9]+$ &&
       "$required_minor" =~ ^[0-9]+$ && "$required_patch" =~ ^[0-9]+$ ]] || return 1

    (( actual_major > required_major ||
       (actual_major == required_major && actual_minor > required_minor) ||
       (actual_major == required_major && actual_minor == required_minor &&
        actual_patch >= required_patch) ))
}

OS_RELEASE_FILE="$(platform_path /etc/os-release)"
PROC_CMDLINE_FILE="$(platform_path /proc/cmdline)"
DEVICE_TREE_MODEL="$(platform_path /proc/device-tree/model)"
DEVICE_ROOT="$(platform_path /dev)"

if read_os_release "$OS_RELEASE_FILE"; then
    :
else
    report WARN "Не удалось прочитать идентификатор ОС; проверьте точный образ Repka OS вручную."
fi

if command -v "$UNAME_COMMAND" >/dev/null 2>&1; then
    MACHINE="$("$UNAME_COMMAND" -m 2>/dev/null || true)"
    KERNEL_RELEASE="$("$UNAME_COMMAND" -r 2>/dev/null || true)"
    WORD_SIZE="$("$GETCONF_COMMAND" LONG_BIT 2>/dev/null || true)"
    if [[ "$MACHINE" == "aarch64" && "$WORD_SIZE" == "64" ]]; then
        report PASS "Архитектура: aarch64, userspace 64-bit."
    else
        report FAIL "Ожидалась нативная 64-битная ARM64-система (aarch64); обнаружено CPU=${MACHINE:-unknown}, LONG_BIT=${WORD_SIZE:-unknown}."
    fi
    if [[ -n "$KERNEL_RELEASE" ]]; then
        report INFO "Ядро Linux: $KERNEL_RELEASE."
    else
        report WARN "Не удалось определить версию ядра Linux."
    fi
else
    report FAIL "Команда uname недоступна; архитектура и версия ядра не проверены."
fi

if [[ -r "$DEVICE_TREE_MODEL" ]]; then
    BOARD_MODEL="$(tr -d '\000' < "$DEVICE_TREE_MODEL" 2>/dev/null || true)"
    if [[ -n "$BOARD_MODEL" ]]; then
        report INFO "Device-tree model: $BOARD_MODEL."
    else
        report WARN "Device-tree model существует, но пуст или недоступен для чтения."
    fi
else
    report WARN "Device-tree model недоступен; модель платы не подтверждена."
fi

if command -v "$CMAKE_COMMAND" >/dev/null 2>&1; then
    CMAKE_VERSION="$("$CMAKE_COMMAND" --version 2>/dev/null | awk 'NR == 1 { print $3 }')"
    if [[ -n "$CMAKE_VERSION" ]] && version_at_least "$CMAKE_VERSION" "3.16.0"; then
        report PASS "CMake $CMAKE_VERSION (требуется не ниже 3.16)."
    else
        report FAIL "CMake $CMAKE_VERSION недоступен или старее минимальной версии 3.16."
    fi
else
    report FAIL "CMake не найден; целевая сборка невозможна."
fi

if command -v "$CXX_COMMAND" >/dev/null 2>&1; then
    COMPILER_VERSION="$("$CXX_COMMAND" --version 2>/dev/null | awk 'NR == 1 { print; exit }')"
    if printf '%s\n' '#if __cplusplus < 201703L' '#error C++17 is required' '#endif' \
        | "$CXX_COMMAND" -std=c++17 -x c++ -fsyntax-only - >/dev/null 2>&1; then
        report PASS "C++ compiler supports C++17: ${COMPILER_VERSION:-version unavailable}."
    else
        report FAIL "Компилятор найден, но не прошёл проверку режима C++17."
    fi

    OPENSSL_CFLAGS=()
    OPENSSL_PKG_VERSION=""
    if command -v "$PKG_CONFIG_COMMAND" >/dev/null 2>&1; then
        OPENSSL_PKG_VERSION="$("$PKG_CONFIG_COMMAND" --modversion openssl 2>/dev/null || true)"
        OPENSSL_CFLAGS_TEXT="$("$PKG_CONFIG_COMMAND" --cflags openssl 2>/dev/null || true)"
        if [[ -n "$OPENSSL_CFLAGS_TEXT" ]]; then
            read -r -a OPENSSL_CFLAGS <<< "$OPENSSL_CFLAGS_TEXT"
        fi
    fi

    OPENSSL_PROBE='#include <openssl/opensslv.h>
#if !defined(OPENSSL_VERSION_MAJOR) || OPENSSL_VERSION_MAJOR < 3
#error OpenSSL 3.0 headers are required
#endif
#include <openssl/ssl.h>
int main() { return 0; }'
    if printf '%s\n' "$OPENSSL_PROBE" \
        | "$CXX_COMMAND" -std=c++17 "${OPENSSL_CFLAGS[@]}" -x c++ -fsyntax-only - >/dev/null 2>&1; then
        if [[ -n "$OPENSSL_PKG_VERSION" ]] &&
           ! version_at_least "$OPENSSL_PKG_VERSION" "3.0.0"; then
            report FAIL "pkg-config указывает OpenSSL $OPENSSL_PKG_VERSION, хотя Gateway требует OpenSSL 3.0+."
        else
            report PASS "Заголовки OpenSSL 3.x доступны компилятору."
        fi
    else
        report FAIL "Не удалось скомпилировать проверку заголовков OpenSSL 3.x; для сборки нужны development headers."
    fi
else
    report FAIL "C++ compiler не найден; проверка C++17/OpenSSL headers невозможна."
fi

if command -v "$PKG_CONFIG_COMMAND" >/dev/null 2>&1; then
    SQLITE_VERSION="$("$PKG_CONFIG_COMMAND" --modversion sqlite3 2>/dev/null || true)"
    if [[ -z "$SQLITE_VERSION" ]]; then
        report INFO "SQLite development metadata не обнаружены; они необязательны для Gateway-only сборки."
    elif version_at_least "$SQLITE_VERSION" "3.31.0"; then
        report INFO "SQLite development version $SQLITE_VERSION доступна для отдельного кандидата persistence."
    else
        report WARN "SQLite $SQLITE_VERSION ниже 3.31; оставьте DOCKSTATION_BUILD_SQLITE_PERSISTENCE=OFF."
    fi
else
    report INFO "pkg-config отсутствует; необязательный SQLite development target не проверялся."
fi

if command -v "$SYSTEMCTL_COMMAND" >/dev/null 2>&1; then
    SYSTEMD_VERSION="$("$SYSTEMCTL_COMMAND" --version 2>/dev/null | awk 'NR == 1 { print $2 }')"
    report INFO "systemd version: ${SYSTEMD_VERSION:-не определена}."
else
    report WARN "systemctl не найден; systemd не подтверждён в текущем образе."
fi

if command -v "$SYSTEMD_ANALYZE_COMMAND" >/dev/null 2>&1; then
    report PASS "systemd-analyze доступен."
    if [[ -z "$SYSTEMD_UNIT_DIR" ]]; then
        SYSTEMD_UNIT_DIR="$(platform_path /usr/local/lib/systemd/system)"
    fi
    BASE_UNIT="$SYSTEMD_UNIT_DIR/dockstation-gateway.service"
    UART_UNIT="$SYSTEMD_UNIT_DIR/dockstation-gateway-uart@.service"
    if [[ -f "$BASE_UNIT" && -f "$UART_UNIT" ]]; then
        if "$SYSTEMD_ANALYZE_COMMAND" verify "$BASE_UNIT" "$UART_UNIT" >/dev/null 2>&1; then
            report PASS "Установленные базовый и UART systemd unit-файлы проходят systemd-analyze verify."
        else
            report FAIL "systemd-analyze verify сообщил об ошибке в установленных unit-файлах."
        fi
    else
        report INFO "Unit-файлы DockStation ещё не установлены; их проверка будет выполнена после CMake install."
    fi
else
    report WARN "systemd-analyze не найден; проверка unit-файлов недоступна."
fi

CONSOLE_DEVICES=()
if [[ -r "$PROC_CMDLINE_FILE" ]]; then
    CMDLINE=""
    IFS= read -r CMDLINE < "$PROC_CMDLINE_FILE" || true
    read -r -a CMDLINE_TOKENS <<< "$CMDLINE"
    for TOKEN in "${CMDLINE_TOKENS[@]}"; do
        case "$TOKEN" in
            console=*)
                CONSOLE_VALUE="${TOKEN#console=}"
                CONSOLE_DEVICES+=("${CONSOLE_VALUE%%,*}")
                ;;
        esac
    done
    if (("${#CONSOLE_DEVICES[@]}" > 0)); then
        report INFO "Параметры kernel console: ${CONSOLE_DEVICES[*]}."
    else
        report INFO "В /proc/cmdline параметр console= не обнаружен."
    fi
else
    report WARN "Не удалось прочитать /proc/cmdline; занятость UART системной консолью не проверена."
fi

shopt -s nullglob
UART_CANDIDATES=(
    "$DEVICE_ROOT"/serial0
    "$DEVICE_ROOT"/serial/by-id/*
    "$DEVICE_ROOT"/serial/by-path/*
    "$DEVICE_ROOT"/ttyS*
    "$DEVICE_ROOT"/ttyAMA*
    "$DEVICE_ROOT"/ttyFIQ*
    "$DEVICE_ROOT"/ttyUSB*
    "$DEVICE_ROOT"/ttyACM*
)
shopt -u nullglob

UART_COUNT=0
SEEN_DEVICES=()
for CANDIDATE in "${UART_CANDIDATES[@]}"; do
    RESOLVED="$CANDIDATE"
    if command -v readlink >/dev/null 2>&1; then
        RESOLVED="$(readlink -f -- "$CANDIDATE" 2>/dev/null || printf '%s' "$CANDIDATE")"
    fi
    [[ -c "$CANDIDATE" || -c "$RESOLVED" ]] || continue

    duplicate=false
    for seen in "${SEEN_DEVICES[@]}"; do
        [[ "$seen" != "$RESOLVED" ]] || duplicate=true
    done
    [[ "$duplicate" == false ]] || continue
    SEEN_DEVICES+=("$RESOLVED")
    ((UART_COUNT += 1))

    DEVICE_METADATA="$(stat -Lc '%F %A %U:%G' "$CANDIDATE" 2>/dev/null || printf 'metadata unavailable')"
    CANDIDATE_NAME="${CANDIDATE##*/}"
    RESOLVED_NAME="${RESOLVED##*/}"
    CONSOLE_MATCH=false
    for CONSOLE_DEVICE in "${CONSOLE_DEVICES[@]}"; do
        if [[ "$CONSOLE_DEVICE" == "$CANDIDATE_NAME" ||
              "$CONSOLE_DEVICE" == "$RESOLVED_NAME" ]]; then
            CONSOLE_MATCH=true
        fi
    done

    if [[ "$CONSOLE_MATCH" == true ]]; then
        report WARN "UART-кандидат $CANDIDATE -> $RESOLVED указан в kernel console; не включайте его в DockStation без отдельного решения."
    else
        report INFO "UART-кандидат: $CANDIDATE -> $RESOLVED ($DEVICE_METADATA). Это только обнаружение узла, не подтверждение распиновки."
    fi
done

if ((UART_COUNT == 0)); then
    report WARN "UART character devices из поддерживаемого списка не обнаружены."
else
    report INFO "Уникальных UART character device кандидатов: $UART_COUNT."
fi
report WARN "Pinmux, Device Tree routing, физический разъём, электрические уровни и фактический UART↔ESP32 обмен этим скриптом не проверяются."
report INFO "Скрипт завершает работу без установки пакетов, изменения /boot, отключения console, изменения прав или запуска/остановки служб."

case "$OVERALL_STATUS" in
    0)
        report PASS "Итог: обязательные программные проверки пройдены; аппаратная проверка всё ещё нужна."
        ;;
    1)
        report WARN "Итог: есть предупреждения; изучите их до включения аппаратного профиля."
        ;;
    *)
        report FAIL "Итог: обнаружена обязательная проблема платформы; сборку/включение Gateway следует остановить до её устранения."
        ;;
esac
exit "$OVERALL_STATUS"
