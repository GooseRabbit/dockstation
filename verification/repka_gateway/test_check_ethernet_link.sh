#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
SCRIPT_PATH="$REPOSITORY_ROOT/deployment/repka/check_network.sh"

if [[ ! -f "$SCRIPT_PATH" ]]; then
    printf 'FAIL: не найден скрипт: %s\n' "$SCRIPT_PATH" >&2
    exit 1
fi

# The function accepts an alternate sysfs root solely to make its read-only
# checks reproducible without physical network hardware.
source "$SCRIPT_PATH"

TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT

make_interface() {
    local root="$1"
    local name="$2"
    local carrier_value="$3"

    mkdir -p "$root/$name/device"
    printf '1\n' > "$root/$name/type"
    if [[ "$carrier_value" != "MISSING" ]]; then
        printf '%s\n' "$carrier_value" > "$root/$name/carrier"
    fi
}

run_case() {
    local description="$1"
    local expected_status="$2"
    local expected_text="$3"
    local sysfs_root="$4"
    local output
    local actual_status

    if output="$(check_ethernet_link "$sysfs_root" 2>&1)"; then
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

# 1. Active Ethernet link.
ACTIVE_ROOT="$TEMP_ROOT/active"
mkdir -p "$ACTIVE_ROOT"
make_interface "$ACTIVE_ROOT" "enp2s0" "1"
run_case "активный физический линк" 0 "[PASS] Ethernet: физический линк активен" "$ACTIVE_ROOT"

# 2. Interface exists, but no cable/link is present.
DOWN_ROOT="$TEMP_ROOT/down"
mkdir -p "$DOWN_ROOT"
make_interface "$DOWN_ROOT" "enp3s0" "0"
run_case "отсутствующий физический линк" 1 "физический линк отсутствует" "$DOWN_ROOT"

# 3. A wireless interface must not be mistaken for wired Ethernet.
WIRELESS_ROOT="$TEMP_ROOT/wireless"
mkdir -p "$WIRELESS_ROOT/wlan0/device" "$WIRELESS_ROOT/wlan0/wireless"
printf '1\n' > "$WIRELESS_ROOT/wlan0/type"
printf '1\n' > "$WIRELESS_ROOT/wlan0/carrier"
run_case "исключение Wi-Fi-интерфейса" 2 "Ethernet-интерфейсы не обнаружены" "$WIRELESS_ROOT"

# 4. Missing carrier information is UNKNOWN, not PASS.
UNKNOWN_ROOT="$TEMP_ROOT/unknown"
mkdir -p "$UNKNOWN_ROOT"
make_interface "$UNKNOWN_ROOT" "enp4s0" "MISSING"
run_case "недоступное состояние carrier" 1 "состояние физического линка недоступно" "$UNKNOWN_ROOT"

# 5. An unexpected carrier value must remain UNKNOWN, not become PASS.
INVALID_ROOT="$TEMP_ROOT/invalid-carrier"
mkdir -p "$INVALID_ROOT"
make_interface "$INVALID_ROOT" "enp4s1" "2"
run_case "некорректное значение carrier" 1 "неожиданный статус carrier: 2" "$INVALID_ROOT"

# 6. A bridge is a logical interface, not a physical Ethernet device.
BRIDGE_ROOT="$TEMP_ROOT/bridge"
mkdir -p "$BRIDGE_ROOT/br0/device" "$BRIDGE_ROOT/br0/bridge"
printf '1\n' > "$BRIDGE_ROOT/br0/type"
printf '1\n' > "$BRIDGE_ROOT/br0/carrier"
run_case "исключение сетевого моста" 2 "Ethernet-интерфейсы не обнаружены" "$BRIDGE_ROOT"

# 7. If multiple Ethernet devices exist, one active link is sufficient for PASS.
MIXED_ROOT="$TEMP_ROOT/mixed"
mkdir -p "$MIXED_ROOT"
make_interface "$MIXED_ROOT" "enp5s0" "0"
make_interface "$MIXED_ROOT" "enp6s0" "1"
run_case "несколько Ethernet-интерфейсов" 0 "активен как минимум на одном интерфейсе (1 из 2)" "$MIXED_ROOT"

# 6. A missing sysfs root is a diagnostic failure, not an empty success.
run_case "недоступный sysfs-каталог" 2 "каталог сетевых интерфейсов недоступен" "$TEMP_ROOT/not-found"

printf '\nВсе тесты check_ethernet_link пройдены.\n'
