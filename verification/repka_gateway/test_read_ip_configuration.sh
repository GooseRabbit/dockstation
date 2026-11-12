#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
SCRIPT_PATH="$REPOSITORY_ROOT/repka/check_network.sh"

if [[ ! -f "$SCRIPT_PATH" ]]; then
    printf 'FAIL: не найден скрипт: %s\n' "$SCRIPT_PATH" >&2
    exit 1
fi

# Source the production function; a fake ip executable makes the checks
# independent of host interfaces and network connectivity.
source "$SCRIPT_PATH"

TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT
FAKE_IP="$TEMP_ROOT/fake-ip"

cat > "$FAKE_IP" <<'FAKE_IP_SCRIPT'
#!/usr/bin/env bash

if [[ "$*" != "-o address show" ]]; then
    printf 'Unexpected arguments: %s\n' "$*" >&2
    exit 64
fi

case "${FAKE_IP_MODE:-}" in
    success)
        printf '%s\n' \
            '1: lo inet 127.0.0.1/8 scope host lo' \
            '2: enp2s0 inet 192.168.10.20/24 brd 192.168.10.255 scope global dynamic enp2s0' \
            '2: enp2s0 inet6 fe80::1234/64 scope link'
        ;;
    empty)
        exit 0
        ;;
    error)
        printf 'simulated command error\n' >&2
        exit 7
        ;;
    malformed)
        printf 'unexpected output line\n'
        ;;
    partial)
        printf '%s\n' \
            '2: enp2s0 inet 192.168.10.20/24 scope global enp2s0' \
            'unexpected output line'
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
    local ip_command="$4"
    local output
    local actual_status

    if output="$(read_ip_configuration "$ip_command" 2>&1)"; then
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

FAKE_IP_MODE=success
export FAKE_IP_MODE
run_case "IPv4 и IPv6 на нескольких интерфейсах" 0 "192.168.10.20/24" "$FAKE_IP"

FAKE_IP_MODE=empty
export FAKE_IP_MODE
run_case "отсутствие назначенных адресов" 1 "не вернула назначенных адресов" "$FAKE_IP"

FAKE_IP_MODE=error
export FAKE_IP_MODE
run_case "ошибка выполнения ip" 1 "кодом 7" "$FAKE_IP"

FAKE_IP_MODE=malformed
export FAKE_IP_MODE
run_case "неразбираемый вывод" 1 "не удалось разобрать строку" "$FAKE_IP"

FAKE_IP_MODE=partial
export FAKE_IP_MODE
run_case "частично корректный вывод" 1 "строк с неизвестным форматом: 1" "$FAKE_IP"

run_case "отсутствующая команда ip" 2 "команда" "$TEMP_ROOT/missing-ip"

printf '\nВсе проверки read_ip_configuration пройдены.\n'
