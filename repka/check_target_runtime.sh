#!/usr/bin/env bash
set -uo pipefail

PLATFORM_ROOT="${DOCKSTATION_PLATFORM_ROOT:-}"
UNAME_COMMAND="${DOCKSTATION_UNAME_COMMAND:-uname}"
GETCONF_COMMAND="${DOCKSTATION_GETCONF_COMMAND:-getconf}"
SYSTEMCTL_COMMAND="${DOCKSTATION_SYSTEMCTL_COMMAND:-systemctl}"
SYSTEMD_ANALYZE_COMMAND="${DOCKSTATION_SYSTEMD_ANALYZE_COMMAND:-systemd-analyze}"
LDD_COMMAND="${DOCKSTATION_LDD_COMMAND:-ldd}"
OVERALL_STATUS=0

platform_path() {
    local path="$1"
    if [[ -n "$PLATFORM_ROOT" ]]; then
        printf '%s%s\n' "${PLATFORM_ROOT%/}" "$path"
    else
        printf '%s\n' "$path"
    fi
}

report() {
    local level="$1"
    shift
    printf '[%s] %s\n' "$level" "$*"
    case "$level" in
        WARN) ((OVERALL_STATUS < 1)) && OVERALL_STATUS=1 ;;
        FAIL) OVERALL_STATUS=2 ;;
    esac
}

OS_RELEASE="$(platform_path /etc/os-release)"
CMDLINE_FILE="$(platform_path /proc/cmdline)"
MODEL_FILE="$(platform_path /proc/device-tree/model)"
DEVICE_ROOT="$(platform_path /dev)"

if [[ -r "$OS_RELEASE" ]]; then
    OS_ID="$(sed -n 's/^ID=//p' "$OS_RELEASE" | head -n 1 | tr -d '"')"
    OS_PRETTY="$(sed -n 's/^PRETTY_NAME=//p' "$OS_RELEASE" | head -n 1 | tr -d '"')"
    report INFO "ОС: ${OS_PRETTY:-unknown} (ID=${OS_ID:-unknown})."
    if [[ "$OS_ID" != "repka" ]]; then
        report WARN "ID ОС не равен repka; runtime ABI bundle требует отдельной проверки на этой системе."
    fi
else
    report WARN "Не удалось прочитать /etc/os-release."
fi

if command -v "$UNAME_COMMAND" >/dev/null 2>&1; then
    MACHINE="$("$UNAME_COMMAND" -m 2>/dev/null || true)"
    KERNEL="$("$UNAME_COMMAND" -r 2>/dev/null || true)"
else
    MACHINE=""
    KERNEL=""
fi
WORD_SIZE="$("$GETCONF_COMMAND" LONG_BIT 2>/dev/null || true)"
if [[ "$MACHINE" == "aarch64" && "$WORD_SIZE" == "64" ]]; then
    report PASS "Платформа: нативная ARM64 userspace 64-bit; kernel ${KERNEL:-unknown}."
else
    report FAIL "Bundle предназначен для aarch64/64-bit; обнаружено machine=${MACHINE:-unknown}, LONG_BIT=${WORD_SIZE:-unknown}."
fi

for tool in "$SYSTEMCTL_COMMAND" "$SYSTEMD_ANALYZE_COMMAND" "$LDD_COMMAND" \
    tar sha256sum install getent groupadd useradd chown chmod mktemp; do
    if command -v "$tool" >/dev/null 2>&1; then
        report PASS "Доступна команда: $tool."
    else
        report FAIL "Отсутствует обязательная runtime-команда: $tool."
    fi
done

if command -v "$SYSTEMCTL_COMMAND" >/dev/null 2>&1; then
    SYSTEMD_VERSION="$("$SYSTEMCTL_COMMAND" --version 2>/dev/null | awk 'NR == 1 { print $2 }')"
    report INFO "systemd: ${SYSTEMD_VERSION:-не определена}."
fi

if command -v "$LDD_COMMAND" >/dev/null 2>&1; then
    LDD_VERSION="$("$LDD_COMMAND" --version 2>&1 | awk 'NR == 1 { print; exit }')"
    report INFO "Runtime linker: ${LDD_VERSION:-ldd доступен}."
fi

if [[ -r "$MODEL_FILE" ]]; then
    MODEL="$(tr -d '\000' < "$MODEL_FILE" 2>/dev/null || true)"
    [[ -z "$MODEL" ]] || report INFO "Device-tree model: $MODEL."
else
    report WARN "Device-tree model не читается; модель платы не подтверждена."
fi

CONSOLES=()
if [[ -r "$CMDLINE_FILE" ]]; then
    CMDLINE=""
    IFS= read -r CMDLINE < "$CMDLINE_FILE" || true
    read -r -a TOKENS <<< "$CMDLINE"
    for token in "${TOKENS[@]}"; do
        case "$token" in
            console=*)
                value="${token#console=}"
                CONSOLES+=("${value%%,*}")
                ;;
        esac
    done
    report INFO "Kernel console: ${CONSOLES[*]:-не задана}."
else
    report WARN "Не удалось прочитать /proc/cmdline; UART console conflict не проверен."
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
for device in "${UART_CANDIDATES[@]}"; do
    resolved="$(readlink -f -- "$device" 2>/dev/null || printf '%s' "$device")"
    [[ -c "$device" || -c "$resolved" ]] || continue

    duplicate=false
    for seen in "${SEEN_DEVICES[@]}"; do
        [[ "$seen" != "$resolved" ]] || duplicate=true
    done
    [[ "$duplicate" == false ]] || continue
    SEEN_DEVICES+=("$resolved")
    ((UART_COUNT += 1))

    name="${device##*/}"
    resolved_name="${resolved##*/}"
    conflict=false
    for console in "${CONSOLES[@]}"; do
        [[ "$console" != "$name" && "$console" != "$resolved_name" ]] || conflict=true
    done
    metadata="$(stat -Lc '%F %A %U:%G' "$device" 2>/dev/null || printf 'metadata unavailable')"
    if [[ "$conflict" == true ]]; then
        report WARN "UART $device занят kernel console; не выбирайте его для DockStation."
    else
        report INFO "UART candidate: $device -> $resolved ($metadata); распиновка и pinmux не подтверждены."
    fi
done

if ((UART_COUNT == 0)); then
    report WARN "UART character devices не обнаружены; это не блокирует установку базового read-only Gateway."
else
    report INFO "Найдено UART character device кандидатов: $UART_COUNT."
fi

report WARN "Pinmux, физический разъём, электрические уровни и UART↔ESP32 обмен требуют проверки на плате."
report INFO "Проверка не запускает apt, не устанавливает библиотеки и не меняет boot, Device Tree, права или systemd state."

case "$OVERALL_STATUS" in
    0) report PASS "Runtime preflight завершён без предупреждений." ;;
    1) report WARN "Runtime preflight завершён с предупреждениями." ;;
    *) report FAIL "Runtime preflight выявил блокирующие требования." ;;
esac
exit "$OVERALL_STATUS"
