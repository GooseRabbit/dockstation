#!/usr/bin/env bash
set -euo pipefail

binary_source="$PWD/build/test_controller_psk_credential_store"
if [[ ! -x "$binary_source" ]]; then
    echo "Credential-store test binary is missing: $binary_source" >&2
    exit 1
fi
if ! getent group dockstation >/dev/null; then
    echo "The dockstation service group is required for this integration test" >&2
    exit 1
fi
if ! getent passwd dockstation >/dev/null; then
    echo "The dockstation service user is required for this integration test" >&2
    exit 1
fi

temporary_root="$(sudo mktemp -d /run/dockstation-credential-test.XXXXXX)"
cleanup() {
    sudo rm -rf -- "$temporary_root"
}
trap cleanup EXIT

sudo chown root:dockstation "$temporary_root"
sudo chmod 0750 "$temporary_root"

# The GitHub workspace may not be traversable by the unprivileged service
# account. Run a root-owned test binary from the same controlled /run tree.
binary="$temporary_root/test_controller_psk_credential_store"
sudo install -o root -g dockstation -m 0750 "$binary_source" "$binary"

prepare_directory() {
    local name="$1"
    local directory="$temporary_root/$name"
    sudo mkdir -- "$directory"
    sudo chown root:dockstation "$directory"
    sudo chmod 0750 "$directory"
    printf '%s\n' "$directory"
}

write_fixture() {
    local file_path="$1"
    local byte_count
    if [[ "$#" -ge 2 ]]; then
        byte_count="$2"
    else
        byte_count=48
    fi
    sudo python3 -c \
        'import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(bytes(range(1, int(sys.argv[2]) + 1)))' \
        "$file_path" "$byte_count"
    sudo chown root:dockstation "$file_path"
    sudo chmod 0640 "$file_path"
}

run_as_service_user() {
    local expectation="$1"
    local directory="$2"
    sudo -u dockstation -- "$binary" "$expectation" "$directory"
}

valid_dir="$(prepare_directory valid)"
write_fixture "$valid_dir/repka-esp32.psk"
run_as_service_user expect-success "$valid_dir"

zero_credentials_dir="$(prepare_directory zero-credentials)"
sudo python3 -c \
    'import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(bytes(48))' \
    "$zero_credentials_dir/repka-esp32.psk"
sudo chown root:dockstation "$zero_credentials_dir/repka-esp32.psk"
sudo chmod 0640 "$zero_credentials_dir/repka-esp32.psk"
run_as_service_user expect-failure "$zero_credentials_dir"

wrong_mode_dir="$(prepare_directory wrong-mode)"
write_fixture "$wrong_mode_dir/repka-esp32.psk"
sudo chmod 0644 "$wrong_mode_dir/repka-esp32.psk"
run_as_service_user expect-failure "$wrong_mode_dir"

wrong_owner_dir="$(prepare_directory wrong-owner)"
write_fixture "$wrong_owner_dir/repka-esp32.psk"
sudo chown dockstation:dockstation "$wrong_owner_dir/repka-esp32.psk"
run_as_service_user expect-failure "$wrong_owner_dir"

wrong_group_dir="$(prepare_directory wrong-group)"
write_fixture "$wrong_group_dir/repka-esp32.psk"
sudo chown root:root "$wrong_group_dir/repka-esp32.psk"
run_as_service_user expect-failure "$wrong_group_dir"

wrong_size_dir="$(prepare_directory wrong-size)"
write_fixture "$wrong_size_dir/repka-esp32.psk" 47
run_as_service_user expect-failure "$wrong_size_dir"

symlink_file_dir="$(prepare_directory symlink-file)"
sudo ln -s "$valid_dir/repka-esp32.psk" "$symlink_file_dir/repka-esp32.psk"
run_as_service_user expect-failure "$symlink_file_dir"

symlink_directory="$temporary_root/symlink-directory"
sudo ln -s "$valid_dir" "$symlink_directory"
run_as_service_user expect-failure "$symlink_directory"

wrong_directory_mode="$(prepare_directory wrong-directory-mode)"
write_fixture "$wrong_directory_mode/repka-esp32.psk"
sudo chmod 0755 "$wrong_directory_mode"
run_as_service_user expect-failure "$wrong_directory_mode"

hard_link_dir="$(prepare_directory hard-link)"
write_fixture "$hard_link_dir/repka-esp32.psk"
sudo ln "$hard_link_dir/repka-esp32.psk" "$hard_link_dir/second-link"
run_as_service_user expect-failure "$hard_link_dir"

echo "Credential-store filesystem acceptance tests passed"
