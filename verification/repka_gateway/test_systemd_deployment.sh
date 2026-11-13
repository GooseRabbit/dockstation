#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${DOCKSTATION_BUILD_DIR:-"$repo_root/build"}"

if [[ ! -f "$build_dir/cmake_install.cmake" ]]; then
    echo "ERROR: configured CMake build not found at $build_dir" >&2
    exit 1
fi

stage_root="$(mktemp -d)"
trap 'rm -rf "$stage_root"' EXIT

cmake --install "$build_dir" --prefix "$stage_root/usr/local" >/dev/null

binary="$stage_root/usr/local/bin/dockstation_linux_bootstrap"
unit="$stage_root/usr/local/lib/systemd/system/dockstation-gateway.service"
config="$stage_root/usr/local/share/dockstation/gateway.conf.example"

[[ -x "$binary" ]] || { echo "ERROR: installed Gateway binary is missing or not executable" >&2; exit 1; }
[[ -f "$unit" ]] || { echo "ERROR: installed systemd unit is missing" >&2; exit 1; }
[[ -f "$config" ]] || { echo "ERROR: installed Gateway config example is missing" >&2; exit 1; }

required_unit_lines=(
    "User=dockstation"
    "Group=dockstation"
    "ExecStart=/usr/local/bin/dockstation_linux_bootstrap --config-file /etc/dockstation/gateway.conf"
    "Restart=on-failure"
    "KillSignal=SIGTERM"
    "NoNewPrivileges=true"
    "PrivateTmp=true"
    "PrivateDevices=true"
    "ProtectSystem=strict"
    "ProtectHome=true"
    "CapabilityBoundingSet="
    "AmbientCapabilities="
    "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK"
    "WantedBy=multi-user.target"
)

for expected in "${required_unit_lines[@]}"; do
    if ! grep -Fqx -- "$expected" "$unit"; then
        echo "ERROR: systemd unit is missing required line: $expected" >&2
        exit 1
    fi
done

grep -Fqx 'api_port=8080' "$config"
grep -Fqx 'log_stderr=true' "$config"

if grep -Eq '^(ExecStart|ListenStream|ListenDatagram)=.*(0\.0\.0\.0|\*:[0-9]+)' "$unit"; then
    echo "ERROR: systemd deployment unexpectedly exposes a wildcard network listener" >&2
    exit 1
fi

echo "PASS: C++ Gateway install layout, config example and hardened systemd unit"
