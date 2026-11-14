#!/usr/bin/env bash
set -euo pipefail

binary="$PWD/build/dockstation-credential-provision"
loader_test_source="$PWD/build/test_controller_psk_credential_store"
credential_directory="/etc/dockstation/credentials"
credential_file="$credential_directory/repka-esp32.psk"

if [[ ! -x "$binary" || ! -x "$loader_test_source" ]]; then
    echo "Credential provisioner or credential-store test binary is missing" >&2
    exit 1
fi
if ! getent group dockstation >/dev/null || ! getent passwd dockstation >/dev/null; then
    echo "The dockstation service user and group are required" >&2
    exit 1
fi
if [[ -e "$credential_directory" || -L "$credential_directory" ]]; then
    echo "Refusing to run: $credential_directory already exists; no existing credentials will be touched" >&2
    exit 1
fi

parent_existed=false
if [[ -d /etc/dockstation ]]; then
    parent_existed=true
fi
binary_root="$(sudo mktemp -d /usr/local/bin/dockstation-credential-provision-test.XXXXXX)"
cleanup() {
    sudo rm -rf -- "$credential_directory" "$binary_root"
    if [[ "$parent_existed" != true ]]; then
        sudo rmdir --ignore-fail-on-non-empty /etc/dockstation 2>/dev/null || true
    fi
}
trap cleanup EXIT

sudo chown root:dockstation "$binary_root"
sudo chmod 0750 "$binary_root"
loader_test="$binary_root/test_controller_psk_credential_store"
sudo install -o root -g dockstation -m 0750 "$loader_test_source" "$loader_test"

"$binary" --help >/dev/null

create_output="$(sudo "$binary")"
if [[ "$create_output" != *"created successfully"* || "$create_output" == *"PSK="* ]]; then
    echo "Provisioner did not return the expected non-secret success message" >&2
    exit 1
fi

directory_stat="$(sudo stat -c '%u:%g:%a' "$credential_directory")"
file_stat="$(sudo stat -c '%u:%g:%a:%h:%s' "$credential_file")"
if [[ "$directory_stat" != "0:$(getent group dockstation | cut -d: -f3):750" ]]; then
    echo "Credential directory ownership/mode mismatch: $directory_stat" >&2
    exit 1
fi
if [[ "$file_stat" != "0:$(getent group dockstation | cut -d: -f3):640:1:48" ]]; then
    echo "Credential file ownership/mode/link-count/size mismatch: $file_stat" >&2
    exit 1
fi

sudo -u dockstation -- "$loader_test" expect-success "$credential_directory"
first_digest="$(sudo sha256sum "$credential_file" | cut -d' ' -f1)"

if sudo "$binary" >/dev/null 2>&1; then
    echo "Provisioner unexpectedly overwrote an existing credential file" >&2
    exit 1
fi
second_digest="$(sudo sha256sum "$credential_file" | cut -d' ' -f1)"
if [[ "$first_digest" != "$second_digest" ]]; then
    echo "Credential file changed after a rejected implicit overwrite" >&2
    exit 1
fi

sudo chmod 0644 "$credential_file"
if sudo "$binary" --replace >/dev/null 2>&1; then
    echo "--replace accepted an existing file with unsafe permissions" >&2
    exit 1
fi
sudo chmod 0640 "$credential_file"
third_digest="$(sudo sha256sum "$credential_file" | cut -d' ' -f1)"
if [[ "$first_digest" != "$third_digest" ]]; then
    echo "Rejected replacement modified the existing credential file" >&2
    exit 1
fi

# Create a real POSIX access ACL xattr without depending on the setfacl package.
sudo python3 - "$credential_file" <<'PY'
import os
import struct
import sys

entries = (
    (0x01, 0x06, 0xFFFFFFFF),  # ACL_USER_OBJ: rw-
    (0x02, 0x04, 65534),       # ACL_USER: r--
    (0x04, 0x04, 0xFFFFFFFF),  # ACL_GROUP_OBJ: r--
    (0x10, 0x04, 0xFFFFFFFF),  # ACL_MASK: r--
    (0x20, 0x00, 0xFFFFFFFF),  # ACL_OTHER: ---
)
acl = struct.pack("<I", 2) + b"".join(
    struct.pack("<HHI", tag, permissions, identifier)
    for tag, permissions, identifier in entries
)
os.setxattr(sys.argv[1], "system.posix_acl_access", acl)
PY
if sudo "$binary" --replace >/dev/null 2>&1; then
    echo "--replace accepted an existing credential file with an access ACL" >&2
    exit 1
fi
sudo python3 - "$credential_file" <<'PY'
import os
import sys
os.removexattr(sys.argv[1], "system.posix_acl_access")
PY
fourth_digest="$(sudo sha256sum "$credential_file" | cut -d' ' -f1)"
if [[ "$first_digest" != "$fourth_digest" ]]; then
    echo "Rejected ACL replacement modified the existing credential file" >&2
    exit 1
fi

sudo "$binary" --replace >/dev/null
fifth_digest="$(sudo sha256sum "$credential_file" | cut -d' ' -f1)"
if [[ "$first_digest" == "$fifth_digest" ]]; then
    echo "--replace did not install a new credential set" >&2
    exit 1
fi
sudo -u dockstation -- "$loader_test" expect-success "$credential_directory"
if sudo find "$credential_directory" -maxdepth 1 -type f -name '.repka-esp32.psk.tmp.*' -print -quit | grep -q .; then
    echo "A temporary credential file was left behind" >&2
    exit 1
fi

echo "Credential provisioner acceptance tests passed"
