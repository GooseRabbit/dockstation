#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
SCRIPT_PATH="$REPOSITORY_ROOT/repka/check_network.sh"

if [[ ! -f "$SCRIPT_PATH" ]]; then
    printf 'FAIL: не найден скрипт: %s\n' "$SCRIPT_PATH" >&2
    exit 1
fi

# Source the production function; the fake ip executable makes the checks
# independent of host routing tables and network connectivity.
source "$SCRIPT_PATH"

TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT
FAKE_IP="$TEMP_ROOT/fake-ip"

cat > "$FAKE_IP" <<'FAKE_IP_SCRIPT'
#!/usr/bin/env bash

case "$*" in
    "-4 -o route show default") family="ipv4" ;;
    "-6 -o route show default") family="ipv6" ;;
    *)
        printf 'Unexpected arguments: %s\n' "$*" >&2
        exit 64
        ;;
esac

case "${FAKE_IP_MODE:-}" in
    both)
        if [[ "$family" == "ipv4" ]]; then
            printf '%s\n' 'default via 192.168.10.1 dev enp2s0 proto dhcp metric 100'
        else
            printf '%s\n' 'default via fe80::1 dev enp2s0 proto ra metric 100'
        fi
        ;;
    ipv4-only)
        if [[ "$family" == "ipv4" ]]; then
            printf '%s\n' 'default via 192.168.10.1 dev enp2s0'
        fi
        ;;
    ipv6-only)
        if [[ "$family" == "ipv6" ]]; then
            printf '%s\n' 'default via fe80::1 dev enp2s0'
        fi
        ;;
    multiple)
        if [[ "$family" == "ipv4" ]]; then
            printf '%s\n' \
                'default via 192.168.10.1 dev enp2s0 metric 100' \
                'default via 192.168.20.1 dev enp3s0 metric 200'
        fi
        ;;
    none)
        exit 0
        ;;
    error)
        printf 'simulated netlink error\n' >&2
        exit 7
        ;;
    partial-error)
        if [[ "$family" == "ipv4" ]]; then
            printf '%s\n' 'default via 192.168.10.1 dev enp2s0'
        else
            printf 'simulated IPv6 error\n' >&2
            exit 7
        fi
        ;;
    malformed)
        if [[ "$family" == "ipv4" ]]; then
            printf '%s\n' '0.0.0.0/0 via 192.168.10.1 dev enp2s0'
        fi
        ;;
    *)
        exit 65
        ;;
esac
FAKE_IP_SCRIPT
chmod +x "$FAKE_IP"

run_case() {
    local description="$1"
    local expected_status="$2"
    local expected_text="$3"
    local output
    local actual_status

    if output="$(check_default_route "$FAKE_IP" 2>&1)"; then
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
        printf 'FAIL: %s: в выводе отсутствует ожидаемый текст "%s"\n%s\n' \
            "$description" "$expected_text" "$output" >&2
        exit 1
    fi

    printf 'PASS: %s\n' "$description"
}

FAKE_IP_MODE="both"
export FAKE_IP_MODE
run_case "маршруты IPv4 и IPv6" 0 "обнаружено маршрутов по умолчанию: 2"

FAKE_IP_MODE="ipv4-only"
export FAKE_IP_MODE
run_case "только маршрут IPv4" 0 "Маршрут по умолчанию IPv4"

FAKE_IP_MODE="ipv6-only"
export FAKE_IP_MODE
run_case "только маршрут IPv6" 0 "Маршрут по умолчанию IPv6"

FAKE_IP_MODE="multiple"
export FAKE_IP_MODE
run_case "несколько default-маршрутов" 0 "обнаружено маршрутов по умолчанию: 2"

FAKE_IP_MODE="none"
export FAKE_IP_MODE
run_case "отсутствие маршрута по умолчанию" 1 "маршруты по умолчанию IPv4/IPv6 не обнаружены"

FAKE_IP_MODE="error"
export FAKE_IP_MODE
run_case "ошибка чтения обоих семейств" 1 "не удалось достоверно определить"

FAKE_IP_MODE="partial-error"
export FAKE_IP_MODE
run_case "ошибка чтения одного семейства" 1 "проверка завершена не полностью"

FAKE_IP_MODE="malformed"
export FAKE_IP_MODE
run_case "неожиданный формат вывода" 1 "неожиданный формат строки"

if output="$(check_default_route "$TEMP_ROOT/missing-ip" 2>&1)"; then
    actual_status=0
else
    actual_status=$?
fi
if [[ "$actual_status" -ne 2 || "$output" != *"не найдена"* ]]; then
    printf 'FAIL: отсутствующая команда ip: ожидался код 2\n%s\n' "$output" >&2
    exit 1
fi
printf 'PASS: отсутствующая команда ip\n'

printf '\nВсе проверки check_default_route пройдены.\n'
