#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
INSTALL_SCRIPT="$ROOT/repka/install_gateway.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
BIN="$TMP/bin"
STAGE="$TMP/stage"
LOG="$TMP/commands.log"
mkdir -p "$BIN" "$STAGE"
: > "$LOG"
cat > "$BIN/preflight" <<'PREFLIGHT'
#!/usr/bin/env bash
echo 'warning only'
exit 1
PREFLIGHT
cat > "$BIN/cmake" <<'CMAKE'
#!/usr/bin/env bash
echo "cmake $*" >> "$DOCKSTATION_TEST_LOG"
if [[ "$1" == --install ]]; then
    dest="${DESTDIR:-}"
    mkdir -p "$dest/usr/local/bin" "$dest/usr/local/lib/systemd/system" "$dest/usr/local/share/dockstation"
    printf '#!/bin/sh\nexit 0\n' > "$dest/usr/local/bin/dockstation_linux_bootstrap"
    chmod +x "$dest/usr/local/bin/dockstation_linux_bootstrap"
    printf '[Service]\nPrivateDevices=true\n' > "$dest/usr/local/lib/systemd/system/dockstation-gateway.service"
    printf '[Service]\nPrivateDevices=false\nDevicePolicy=closed\nDeviceAllow=/dev/%%i rw\n' > "$dest/usr/local/lib/systemd/system/dockstation-gateway-uart@.service"
    printf 'api_port=8080\nlog_stderr=true\n' > "$dest/usr/local/share/dockstation/gateway.conf.example"
fi
CMAKE
cat > "$BIN/systemd-analyze" <<'VERIFY'
#!/usr/bin/env bash
echo "verify $*" >> "$DOCKSTATION_TEST_LOG"
[[ "$1" == verify && -f "$2" && -f "$3" ]]
VERIFY
chmod +x "$BIN/"*
export PATH="$BIN:/usr/bin:/bin"
export DOCKSTATION_PREFLIGHT_SCRIPT="$BIN/preflight"
export DOCKSTATION_CMAKE_COMMAND="$BIN/cmake"
export DOCKSTATION_SYSTEMD_ANALYZE_COMMAND="$BIN/systemd-analyze"
export DOCKSTATION_INSTALL_ROOT="$STAGE"
export DOCKSTATION_BUILD_DIR="$TMP/build"
export DOCKSTATION_TEST_LOG="$LOG"
bash "$INSTALL_SCRIPT"
[[ -x "$STAGE/usr/local/bin/dockstation_linux_bootstrap" ]]
[[ -f "$STAGE/usr/local/lib/systemd/system/dockstation-gateway.service" ]]
[[ -f "$STAGE/usr/local/lib/systemd/system/dockstation-gateway-uart@.service" ]]
[[ -f "$STAGE/etc/dockstation/gateway.conf" ]]
grep -Fqx 'api_port=8080' "$STAGE/etc/dockstation/gateway.conf"
grep -Fq -- '-DDOCKSTATION_BUILD_SQLITE_PERSISTENCE=OFF' "$LOG"
grep -Fq -- '-DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=TRUE' "$LOG"
printf 'operator_setting=preserve-me\n' > "$STAGE/etc/dockstation/gateway.conf"
bash "$INSTALL_SCRIPT"
grep -Fqx 'operator_setting=preserve-me' "$STAGE/etc/dockstation/gateway.conf"
if grep -Eiq 'apt(-get)? (update|upgrade|install)|do-release-upgrade' "$INSTALL_SCRIPT"; then
    echo "FAIL: installer contains package manager actions." >&2
    exit 1
fi
printf 'PASS: staged installer, SQLite-free Gateway build, config preservation and no OS updates.\n'
