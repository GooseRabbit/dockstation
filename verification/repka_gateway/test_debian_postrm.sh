#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT/packaging/debian/postrm"
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT
BIN="$TMP/bin"
LOG="$TMP/helper.log"
mkdir -p "$BIN"
: > "$LOG"

cat > "$BIN/deb-systemd-helper" <<'SH'
#!/bin/sh
printf 'helper %s\n' "$*" >> "$DOCKSTATION_POSTRM_TEST_LOG"
SH
cat > "$BIN/systemctl" <<'SH'
#!/bin/sh
printf 'systemctl %s\n' "$*" >> "$DOCKSTATION_POSTRM_TEST_LOG"
SH
cat > "$BIN/rmdir" <<'SH'
#!/bin/sh
printf 'rmdir %s\n' "$*" >> "$DOCKSTATION_POSTRM_TEST_LOG"
SH
chmod +x "$BIN/deb-systemd-helper" "$BIN/systemctl" "$BIN/rmdir"
export PATH="$BIN:/usr/bin:/bin"
export DOCKSTATION_POSTRM_TEST_LOG="$LOG"

sh "$SCRIPT" remove
grep -Fqx 'helper disable dockstation-gateway.service' "$LOG"
if grep -Fq 'rmdir ' "$LOG"; then
    echo "FAIL: remove must retain the conffile directory." >&2
    exit 1
fi
if grep -Fq 'helper purge ' "$LOG"; then
    echo "FAIL: remove must not purge deb-systemd-helper state." >&2
    exit 1
fi

: > "$LOG"
sh "$SCRIPT" purge
grep -Fqx 'helper purge dockstation-gateway.service' "$LOG"
if grep -Fq 'helper disable ' "$LOG"; then
    echo "FAIL: purge must use the purge action, not disable." >&2
    exit 1
fi
grep -Fqx 'systemctl daemon-reload' "$LOG"
grep -Fqx 'rmdir /etc/dockstation' "$LOG"

if grep -Eq 'rm .*(/var/lib/dockstation|dockstation\.sqlite3)' "$SCRIPT"; then
    echo "FAIL: postrm must not remove persistent station data." >&2
    exit 1
fi

printf 'PASS: Debian remove/purge helper state and persistent-data policy.\n'
