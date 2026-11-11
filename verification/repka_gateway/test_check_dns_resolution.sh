#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
SCRIPT_PATH="$REPOSITORY_ROOT/deployment/repka/check_network.sh"

if [[ ! -f "$SCRIPT_PATH" ]]; then
    printf 'FAIL: не найден скрипт: %s\n' "$SCRIPT_PATH" >&2
    exit 1
fi

source "$SCRIPT_PATH"

ORIGINAL_PATH="$PATH"
TEMP_ROOT="$(mktemp -d)"
trap 'PATH="$ORIGINAL_PATH"; rm -rf "$TEMP_ROOT"' EXIT
FAKE_BIN="$TEMP_ROOT/bin"
GETENT_ONLY="$TEMP_ROOT/getent-only"
EMPTY_BIN="$TEMP_ROOT/empty-bin"
mkdir -p "$FAKE_BIN" "$GETENT_ONLY" "$EMPTY_BIN"

cat > "$FAKE_BIN/getent" <<'FAKE_GETENT'
#!/bin/bash
if [[ "$#" -ne 2 || "$1" != "ahosts" || "$2" != "station-check.example" ]]; then
    printf 'Unexpected getent arguments: %s\n' "$*" >&2
    exit 64
fi
case "${FAKE_GETENT_MODE:-success}" in
    success)
        printf '%s\n' '192.0.2.10 STREAM station-check.example' '2001:db8::10 STREAM station-check.example'
        ;;
    unresolved)
        printf '%s\n' 'Name or service not known' >&2
        exit 2
        ;;
    error)
        printf '%s\n' 'simulated resolver failure' >&2
        exit 1
        ;;
    empty)
        exit 0
        ;;
    *)
        printf 'Unknown fake getent mode: %s\n' "$FAKE_GETENT_MODE" >&2
        exit 65
        ;;
esac
FAKE_GETENT

cat > "$FAKE_BIN/timeout" <<'FAKE_TIMEOUT'
#!/bin/bash
if [[ "$#" -lt 5 || "$1" != "--signal=TERM" || "$2" != "--kill-after=1s" ]]; then
    printf 'Unexpected timeout arguments: %s\n' "$*" >&2
    exit 64
fi
shift 2
duration="$1"
shift
if [[ ! "$duration" =~ ^[1-9][0-9]*s$ ]]; then
    printf 'Unexpected timeout value: %s\n' "$duration" >&2
    exit 64
fi
if [[ "${FAKE_TIMEOUT_MODE:-normal}" == "timeout" ]]; then
    printf '%s\n' 'simulated timeout' >&2
    exit 124
fi
exec "$@"
FAKE_TIMEOUT

cp "$FAKE_BIN/getent" "$GETENT_ONLY/getent"
chmod +x "$FAKE_BIN/getent" "$FAKE_BIN/timeout" "$GETENT_ONLY/getent"

run_case() {
    local description="$1"
    local expected_status="$2"
    local expected_text="$3"
    shift 3
    local output
    local actual_status

    if output="$("$@" 2>&1)"; then
        actual_status=0
    else
        actual_status=$?
    fi

    if [[ "$actual_status" -ne "$expected_status" ]]; then
        printf 'FAIL: %s: ожидался код %s, получен %s\n%s\n' \
            "$description" "$expected_status" "$actual_status" "$output" >&2
        exit 1
    fi

    if [[ "$output" != *"$expected_text"* ]]; then
        printf 'FAIL: %s: в выводе отсутствует "%s"\n%s\n' \
            "$description" "$expected_text" "$output" >&2
        exit 1
    fi

    printf 'PASS: %s\n' "$description"
}

export PATH="$FAKE_BIN:$ORIGINAL_PATH"
export FAKE_GETENT_MODE="success"
export FAKE_TIMEOUT_MODE="normal"
run_case "успешное разрешение IPv4/IPv6" 0 "разрешено системным механизмом" check_dns_resolution "station-check.example"
export DOCKSTATION_DNS_CHECK_NAME="station-check.example"
run_case "контрольное имя из переменной окружения" 0 "разрешено системным механизмом" check_dns_resolution
unset DOCKSTATION_DNS_CHECK_NAME

export FAKE_GETENT_MODE="unresolved"
run_case "имя не разрешается" 1 "не удалось разрешить имя" check_dns_resolution "station-check.example"

export FAKE_GETENT_MODE="error"
run_case "ошибка системного resolver" 1 "simulated resolver failure" check_dns_resolution "station-check.example"

export FAKE_GETENT_MODE="empty"
run_case "пустой успешный ответ" 1 "не вернула адресов" check_dns_resolution "station-check.example"

export FAKE_GETENT_MODE="success"
export FAKE_TIMEOUT_MODE="timeout"
run_case "истечение тайм-аута" 1 "превысила тайм-аут" check_dns_resolution "station-check.example" 2

export FAKE_TIMEOUT_MODE="normal"
run_case "контрольное имя не задано" 1 "DOCKSTATION_DNS_CHECK_NAME" check_dns_resolution ""
run_case "некорректное контрольное имя" 2 "некорректное контрольное имя" check_dns_resolution "bad..example"
run_case "некорректный тайм-аут" 2 "от 1 до 60 секунд" check_dns_resolution "station-check.example" 0

PATH="$EMPTY_BIN"
export PATH
run_case "отсутствует getent" 2 "команда getent не найдена" check_dns_resolution "station-check.example"

PATH="$GETENT_ONLY"
export PATH
run_case "отсутствует timeout" 2 "команда timeout не найдена" check_dns_resolution "station-check.example"

printf '\nВсе проверки check_dns_resolution пройдены.\n'
