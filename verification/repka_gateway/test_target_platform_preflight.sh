#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
CHECK_SCRIPT="$REPOSITORY_ROOT/repka/check_target_platform.sh"
TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT

FAKE_ROOT="$TEMP_ROOT/root"
FAKE_BIN="$TEMP_ROOT/bin"
mkdir -p "$FAKE_BIN" "$FAKE_ROOT/etc" "$FAKE_ROOT/proc" \
    "$FAKE_ROOT/proc/device-tree" "$FAKE_ROOT/dev" \
    "$FAKE_ROOT/usr/local/lib/systemd/system"

cat > "$FAKE_ROOT/etc/os-release" <<'OS_RELEASE'
NAME="Repka OS"
PRETTY_NAME="Repka OS 5 Basic"
ID=repka
VERSION_ID="1.0.6"
OS_RELEASE
printf '%s\n' 'console=ttyS0,115200 root=/dev/mmcblk0p2' > "$FAKE_ROOT/proc/cmdline"
printf 'Repka Pi 5\0' > "$FAKE_ROOT/proc/device-tree/model"
ln -s /dev/null "$FAKE_ROOT/dev/ttyS0"
touch "$FAKE_ROOT/usr/local/lib/systemd/system/dockstation-gateway.service"
touch "$FAKE_ROOT/usr/local/lib/systemd/system/dockstation-gateway-uart@.service"

cat > "$FAKE_BIN/uname" <<'UNAME'
#!/usr/bin/env bash
case "${1:-}" in
    -m) printf '%s\n' "${FAKE_UNAME_MACHINE:-aarch64}" ;;
    -r) printf '%s\n' '6.1.115.1-repka-pi5' ;;
    *) exit 64 ;;
esac
UNAME
cat > "$FAKE_BIN/getconf" <<'GETCONF'
#!/usr/bin/env bash
[[ "${1:-}" == LONG_BIT ]] || exit 64
printf '%s\n' '64'
GETCONF
cat > "$FAKE_BIN/cmake" <<'CMAKE'
#!/usr/bin/env bash
[[ "${1:-}" == --version ]] || exit 64
printf '%s\n' 'cmake version 3.28.3'
CMAKE
cat > "$FAKE_BIN/c++" <<'CXX'
#!/usr/bin/env bash
if [[ "${1:-}" == --version ]]; then
    printf '%s\n' 'g++ (test fixture) 14.2.0'
else
    cat >/dev/null
fi
CXX
cat > "$FAKE_BIN/pkg-config" <<'PKG_CONFIG'
#!/usr/bin/env bash
case "${1:-}:${2:-}" in
    --cflags:openssl) printf '\n' ;;
    --modversion:openssl) printf '%s\n' "${FAKE_OPENSSL_VERSION:-3.0.13}" ;;
    --modversion:sqlite3)
        [[ "${FAKE_SQLITE_VERSION:-3.45.1}" != missing ]] || exit 1
        printf '%s\n' "${FAKE_SQLITE_VERSION:-3.45.1}"
        ;;
    *) exit 1 ;;
esac
PKG_CONFIG
cat > "$FAKE_BIN/systemctl" <<'SYSTEMCTL'
#!/usr/bin/env bash
[[ "${1:-}" == --version ]] || exit 64
printf '%s\n' 'systemd 255 (test fixture)'
SYSTEMCTL
cat > "$FAKE_BIN/systemd-analyze" <<'SYSTEMD_ANALYZE'
#!/usr/bin/env bash
case "${1:-}" in
    --version) printf '%s\n' 'systemd 255 (test fixture)' ;;
    verify) exit "${FAKE_SYSTEMD_VERIFY_STATUS:-0}" ;;
    *) exit 64 ;;
esac
SYSTEMD_ANALYZE
chmod +x "$FAKE_BIN"/*

export PATH="$FAKE_BIN:/usr/bin:/bin"
export CXX="$FAKE_BIN/c++"
export DOCKSTATION_PLATFORM_ROOT="$FAKE_ROOT"
export DOCKSTATION_SYSTEMD_UNIT_DIR="$FAKE_ROOT/usr/local/lib/systemd/system"

run_case() {
    local description="$1"
    local expected_status="$2"
    local expected_text="$3"
    shift 3
    local output actual_status
    if output="$(env "$@" bash "$CHECK_SCRIPT" 2>&1)"; then
        actual_status=0
    else
        actual_status=$?
    fi
    if [[ "$actual_status" -ne "$expected_status" || "$output" != *"$expected_text"* ]]; then
        printf 'FAIL: %s (expected=%s actual=%s)\n%s\n' \
            "$description" "$expected_status" "$actual_status" "$output" >&2
        exit 1
    fi
    printf 'PASS: %s\n' "$description"
}

run_case \
    "target preflight detects ARM64/OpenSSL/systemd and reports UART console conflict without mutation" \
    1 \
    "указан в kernel console" \
    "FAKE_UNAME_MACHINE=aarch64"

run_case \
    "target preflight rejects non-ARM64 architecture" \
    2 \
    "Ожидалась нативная 64-битная ARM64-система" \
    "FAKE_UNAME_MACHINE=x86_64"

run_case \
    "target preflight rejects SQLite older than 3.31 for persistent Gateway installation" \
    2 \
    "SQLite 3.28.0 ниже обязательной версии 3.31" \
    "FAKE_UNAME_MACHINE=aarch64" \
    "FAKE_SQLITE_VERSION=3.28.0"

run_case \
    "target preflight requires SQLite development metadata for the full installation" \
    2 \
    "SQLite development metadata не обнаружены" \
    "FAKE_UNAME_MACHINE=aarch64" \
    "FAKE_SQLITE_VERSION=missing"

run_case \
    "target preflight treats OpenSSL older than 3.0 as a blocker" \
    2 \
    "pkg-config указывает OpenSSL" \
    "FAKE_UNAME_MACHINE=aarch64" \
    "FAKE_OPENSSL_VERSION=1.1.1"

printf '\nAll target platform preflight tests passed.\n'
