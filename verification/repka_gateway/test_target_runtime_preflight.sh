#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/../.." && pwd)"
CHECK="$ROOT/repka/check_target_runtime.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAKE="$TMP/bin"
SYS="$TMP/root"
mkdir -p "$FAKE" "$SYS/etc" "$SYS/proc" "$SYS/proc/device-tree" "$SYS/dev"
cat > "$SYS/etc/os-release" <<'OS'
PRETTY_NAME="Repka OS 5"
ID=repka
OS
printf '%s\n' 'console=ttyS0,115200 root=/dev/mmcblk0p2' > "$SYS/proc/cmdline"
printf 'Repka Pi 5\0' > "$SYS/proc/device-tree/model"

cat > "$FAKE/uname" <<'SH'
#!/usr/bin/env bash
case "$1" in
  -m) printf '%s\n' "${FAKE_MACHINE:-aarch64}" ;;
  -r) printf '%s\n' '6.1.115.1-repka-pi5' ;;
  *) exit 64 ;;
esac
SH
cat > "$FAKE/getconf" <<'SH'
#!/usr/bin/env bash
printf '%s\n' '64'
SH
for name in systemctl systemd-analyze; do
    cat > "$FAKE/$name" <<'SH'
#!/usr/bin/env bash
printf '%s\n' 'systemd 255 (test fixture)'
SH
done
cat > "$FAKE/ldd" <<'SH'
#!/usr/bin/env bash
printf '%s\n' 'ldd (GNU libc) 2.39'
SH
for name in tar sha256sum install getent groupadd useradd chown chmod mktemp; do
    printf '#!/usr/bin/env bash\nexit 0\n' > "$FAKE/$name"
done
chmod +x "$FAKE/"*

export PATH="$FAKE:/usr/bin:/bin"
export DOCKSTATION_PLATFORM_ROOT="$SYS"
export DOCKSTATION_UNAME_COMMAND="$FAKE/uname"
export DOCKSTATION_GETCONF_COMMAND="$FAKE/getconf"
export DOCKSTATION_SYSTEMCTL_COMMAND="$FAKE/systemctl"
export DOCKSTATION_SYSTEMD_ANALYZE_COMMAND="$FAKE/systemd-analyze"
export DOCKSTATION_LDD_COMMAND="$FAKE/ldd"

run_case() {
    local expected_status="$1"
    local expected_text="$2"
    shift 2
    local output status
    if output="$(env "$@" bash "$CHECK" 2>&1)"; then status=0; else status=$?; fi
    if [[ "$status" -ne "$expected_status" || "$output" != *"$expected_text"* ]]; then
        printf 'FAIL: expected status %s and text %s; got %s\n%s\n' \
            "$expected_status" "$expected_text" "$status" "$output" >&2
        exit 1
    fi
}
run_case 1 "UART character devices не обнаружены" "FAKE_MACHINE=aarch64"
run_case 2 "Bundle предназначен для aarch64/64-bit" "FAKE_MACHINE=x86_64"
run_case 2 "Отсутствует обязательная runtime-команда" "DOCKSTATION_LDD_COMMAND=not-a-real-ldd"
printf 'PASS: target runtime preflight architecture and dependency-tool checks.\n'
