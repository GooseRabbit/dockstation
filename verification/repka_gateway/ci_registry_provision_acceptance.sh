#!/usr/bin/env bash
set -euo pipefail

registry="/usr/local/bin/dockstation-registry-provision"
credential_provisioner="/usr/local/bin/dockstation-credential-provision"
credential_directory="/etc/dockstation/credentials"
credential_file="$credential_directory/repka-esp32.psk"
gateway_config="/etc/dockstation/gateway.conf"
database="/var/lib/dockstation/dockstation.sqlite3"
temporary_root="$(mktemp -d)"
credentials_created=false
config_changed=false
config_backup="$temporary_root/gateway.conf"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

restore_config() {
    if [[ "$config_changed" == true && -f "$config_backup" ]]; then
        sudo install -o root -g dockstation -m 0640 \
            "$config_backup" "$gateway_config"
        config_changed=false
    fi
}

cleanup() {
    restore_config || true
    if [[ "$credentials_created" == true ]]; then
        sudo rm -rf -- "$credential_directory"
    fi
    rm -rf -- "$temporary_root"
}
trap cleanup EXIT

[[ -x "$registry" ]] || fail "Registry provisioner binary is not installed."
[[ -x "$credential_provisioner" ]] || fail "Credential provisioner binary is not installed."
[[ -f "$database" && ! -L "$database" ]] || fail "Gateway SQLite database is not initialized."
[[ ! -e "$credential_directory" && ! -L "$credential_directory" ]] ||
    fail "Refusing to run: CI credential directory unexpectedly exists."

# A generated test PSK is local to this disposable CI runner. It is never sent
# to the registry CLI or printed in logs; only the non-secret identity is used.
# Start with the installer-created private StateDirectory but no DB file. The
# provisioner must initialize SQLite itself under the dockstation service UID.
sudo rm -f -- "$database"
[[ ! -e "$database" && ! -L "$database" ]] ||
    fail "Could not remove the initial Gateway DB for the first-open provisioning test."
credentials_created=true
credential_output="$(sudo "$credential_provisioner" 2>&1)"
[[ "$credential_output" == *"Local Repka credentials created successfully"* ]] ||
    fail "Credential provisioner did not create the CI fixture."

identity_hex="$(sudo python3 - "$credential_file" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
if len(data) != 48:
    raise SystemExit("credential fixture must be exactly 48 bytes")
if not any(data[:32]) or not any(data[32:]):
    raise SystemExit("credential fixture contains an all-zero PSK or identity")
print(data[32:].hex())
PY
)"
psk_hex="$(sudo python3 - "$credential_file" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
print(data[:32].hex())
PY
)"

run_registry() {
    sudo -u dockstation -- "$registry" "$@"
}

expect_rejected() {
    local expected_status="$1"
    shift
    local output status
    if output="$(run_registry "$@" 2>&1)"; then
        fail "Registry operation unexpectedly succeeded: $*"
    else
        status=$?
    fi
    [[ "$status" -eq "$expected_status" ]] ||
        fail "Registry operation returned $status instead of $expected_status: $*"
    [[ "$output" != *"$psk_hex"* ]] ||
        fail "Registry utility exposed the PSK secret."
}

assert_db() {
    local mode="$1"
    sudo python3 - "$database" "$credential_file" "$mode" <<'PY'
from pathlib import Path
import sqlite3
import sys

database, credential_file, mode = sys.argv[1:]
credential = Path(credential_file).read_bytes()
identity = credential[32:]
connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
try:
    source_rows = connection.execute(
        "SELECT station_id, source, registration_revision, peer_identity, state "
        "FROM source_registrations"
    ).fetchall()
    subsystem_rows = connection.execute(
        "SELECT station_id, subsystem_id, source, registration_revision, state "
        "FROM subsystem_registrations"
    ).fetchall()
    cursors = connection.execute("SELECT source FROM source_cursors").fetchall()

    if mode == "empty":
        assert source_rows == [], source_rows
        assert subsystem_rows == [], subsystem_rows
        assert cursors == [], cursors
    elif mode == "source":
        assert len(source_rows) == 1, source_rows
        assert source_rows[0] == (
            "station-ci", "esp32.main", "ci-r1", identity, 1
        ), source_rows
        assert subsystem_rows == [], subsystem_rows
        assert cursors == [], cursors
    elif mode == "complete":
        assert len(source_rows) == 1, source_rows
        assert source_rows[0] == (
            "station-ci", "esp32.main", "ci-r1", identity, 1
        ), source_rows
        assert subsystem_rows == [(
            "station-ci", "weather.temperature",
            "esp32.main", "ci-r1", 1
        )], subsystem_rows
        assert cursors == [], cursors
    else:
        raise AssertionError(f"unknown assertion mode: {mode}")
finally:
    connection.close()
PY
}

# The utility must refuse root invocation; ordinary operations use the service UID.
if root_output="$(sudo "$registry" source --revision ci-r1 2>&1)"; then
    fail "Registry utility unexpectedly accepted a root invocation."
else
    root_status=$?
fi
[[ "$root_status" -eq 1 ]] ||
    fail "Root invocation returned $root_status instead of 1."
[[ "$root_output" != *"$psk_hex"* ]] ||
    fail "Root-invocation diagnostic exposed the PSK secret."

# A subsystem cannot be registered before the exact source identity is active.
expect_rejected 3 subsystem --subsystem-id weather.temperature --revision ci-r1
assert_db empty
[[ "$(sudo stat -c '%U:%G' "$database")" == "dockstation:dockstation" ]] ||
    fail "Provisioner-created DB has an unexpected owner/group."
[[ "$(sudo stat -c '%a' "$database")" == "600" ]] ||
    fail "Provisioner-created DB does not have mode 0600."

source_preview="$(run_registry source --revision ci-r1 2>&1)"
[[ "$source_preview" == *"PREVIEW ONLY"* ]] ||
    fail "Source preview did not report preview-only mode."
[[ "$source_preview" == *"$identity_hex"* ]] ||
    fail "Source preview did not display the registered opaque identity."
[[ "$source_preview" != *"$psk_hex"* ]] ||
    fail "Source preview exposed the PSK secret."
assert_db empty

source_apply="$(run_registry source --revision ci-r1 --apply 2>&1)"
[[ "$source_apply" == *"SUCCESS: registration created atomically"* ]] ||
    fail "Source registration was not confirmed."
[[ "$source_apply" != *"$psk_hex"* ]] ||
    fail "Source registration output exposed the PSK secret."
assert_db source
expect_rejected 3 source --revision ci-r2 --apply
assert_db source
expect_rejected 2 source --revision "bad revision"
assert_db source

subsystem_preview="$(run_registry subsystem \
    --subsystem-id weather.temperature --revision ci-r1 2>&1)"
[[ "$subsystem_preview" == *"PREVIEW ONLY"* ]] ||
    fail "Subsystem preview did not report preview-only mode."
[[ "$subsystem_preview" != *"$psk_hex"* ]] ||
    fail "Subsystem preview exposed the PSK secret."
assert_db source

subsystem_apply="$(run_registry subsystem \
    --subsystem-id weather.temperature --revision ci-r1 --apply 2>&1)"
[[ "$subsystem_apply" == *"SUCCESS: registration created atomically"* ]] ||
    fail "Subsystem registration was not confirmed."
[[ "$subsystem_apply" != *"$psk_hex"* ]] ||
    fail "Subsystem registration output exposed the PSK secret."
assert_db complete
expect_rejected 3 subsystem \
    --subsystem-id weather.temperature --revision ci-r2 --apply
assert_db complete

# Prove that a config Station ID mismatch cannot attach the existing identity
# to another station. The original config is restored even if this check fails.
sudo cp -p -- "$gateway_config" "$config_backup"
sudo sed -i 's/^station_id=station-ci$/station_id=station-ci-mismatch/' "$gateway_config"
config_changed=true
expect_rejected 3 subsystem \
    --subsystem-id weather.pressure --revision ci-r1 --apply
restore_config
assert_db complete

echo "Registry provisioner acceptance tests passed."
