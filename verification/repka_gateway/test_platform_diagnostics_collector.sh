#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
COLLECTOR="$REPOSITORY_ROOT/repka/collect_platform_diagnostics.sh"
TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT

FAKE_ROOT="$TEMP_ROOT/root"
OUTPUT_DIR="$TEMP_ROOT/report"
mkdir -p "$FAKE_ROOT/etc" "$FAKE_ROOT/proc/device-tree/aliases" \
    "$FAKE_ROOT/proc/device-tree/serial@fe650000" \
    "$FAKE_ROOT/proc/device-tree/i2c@feaa0000" \
    "$FAKE_ROOT/proc" "$FAKE_ROOT/sys/class/tty/ttyS0/device" \
    "$FAKE_ROOT/sys/class/thermal/thermal_zone0" "$FAKE_ROOT/dev"

cat > "$FAKE_ROOT/etc/os-release" <<'OS_RELEASE'
NAME="Repka OS"
PRETTY_NAME="Repka OS 5 Basic 1.0.6"
ID=repka
VERSION_ID="1.0.6"
UNTRUSTED=$(touch "PLACEHOLDER_MARKER")
OS_RELEASE
sed -i "s|PLACEHOLDER_MARKER|$TEMP_ROOT/must-not-execute|" "$FAKE_ROOT/etc/os-release"

printf '%s\n' 'console=ttyS0,115200 root=/dev/mmcblk0p2 psk=NEVER_COPY_THIS_SECRET token=NEVER_COPY_THIS_TOKEN iommu=on quiet' > "$FAKE_ROOT/proc/cmdline"
cat > "$FAKE_ROOT/proc/cpuinfo" <<'CPUINFO'
processor : 0
model name : ARMv8 Processor rev 1
Hardware : Rockchip RK3588
Serial : 0123456789abcdef
CPU implementer : 0x41
CPU part : 0xd0c
CPUINFO
printf '%s\n' 'dw_apb_uart 24576 1 - Live 0x00000000' > "$FAKE_ROOT/proc/modules"

printf 'Repka Pi 5\0' > "$FAKE_ROOT/proc/device-tree/model"
printf 'rockchip,rk3588\0repka,pi5\0' > "$FAKE_ROOT/proc/device-tree/compatible"
printf '/serial@fe650000\0' > "$FAKE_ROOT/proc/device-tree/aliases/serial0"
printf 'rockchip,rk3588-uart\0' > "$FAKE_ROOT/proc/device-tree/serial@fe650000/compatible"
printf 'okay\0' > "$FAKE_ROOT/proc/device-tree/serial@fe650000/status"
printf 'serial\0' > "$FAKE_ROOT/proc/device-tree/serial@fe650000/name"
printf 'temperature\0' > "$FAKE_ROOT/sys/class/thermal/thermal_zone0/type"
printf '42000\n' > "$FAKE_ROOT/sys/class/thermal/thermal_zone0/temp"
ln -s /dev/null "$FAKE_ROOT/dev/ttyS0"

DOCKSTATION_DIAG_ROOT="$FAKE_ROOT" DOCKSTATION_DIAG_SKIP_PROBES=1 \
    bash "$COLLECTOR" "$OUTPUT_DIR" > "$TEMP_ROOT/collector.log"

grep -Fq 'Repka OS 5 Basic 1.0.6' "$OUTPUT_DIR/01_SYSTEM.txt"
grep -Fq 'model=Repka Pi 5' "$OUTPUT_DIR/04_DEVICE_TREE.txt"
grep -Fq 'compatible=rockchip,rk3588 repka,pi5' "$OUTPUT_DIR/04_DEVICE_TREE.txt"
grep -Fq 'serial0=/serial@fe650000' "$OUTPUT_DIR/04_DEVICE_TREE.txt"
grep -Fq '/serial@fe650000' "$OUTPUT_DIR/04_DEVICE_TREE.txt"
grep -Fq 'ttyS0' "$OUTPUT_DIR/06_INTERFACES.txt"
grep -Fq 'thermal_zone0 type=temperature temp_millidegrees=42000' "$OUTPUT_DIR/06_INTERFACES.txt"
grep -Fq 'dw_apb_uart' "$OUTPUT_DIR/03_KERNEL_MODULES.txt"
grep -Fq 'console=ttyS0,115200' "$OUTPUT_DIR/05_BOOT.txt"
grep -Fq 'iommu=on' "$OUTPUT_DIR/05_BOOT.txt"

if grep -R -E 'NEVER_COPY_THIS_SECRET|NEVER_COPY_THIS_TOKEN|0123456789abcdef|root=/dev/mmcblk0p2|UNTRUSTED=' "$OUTPUT_DIR"; then
    echo 'FAIL: sensitive or non-allowlisted fixture value leaked into the report' >&2
    exit 1
fi
[[ ! -e "$TEMP_ROOT/must-not-execute" ]]
[[ -s "$OUTPUT_DIR.tar.gz" ]]
[[ -s "$OUTPUT_DIR.tar.gz.sha256" ]]
tar -tzf "$OUTPUT_DIR.tar.gz" | grep -Fq 'report/04_DEVICE_TREE.txt'

set +e
DOCKSTATION_DIAG_ROOT="$FAKE_ROOT" DOCKSTATION_DIAG_SKIP_PROBES=1 \
    bash "$COLLECTOR" "$OUTPUT_DIR" > "$TEMP_ROOT/repeat.log" 2>&1
repeat_status=$?
set -e
[[ "$repeat_status" -eq 2 ]]
grep -Fq 'refusing to overwrite' "$TEMP_ROOT/repeat.log"

printf 'PASS: collector records platform data, filters secrets, archives output and refuses overwrite.\n'
