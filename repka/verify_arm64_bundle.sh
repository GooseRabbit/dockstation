#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

info() {
    printf '[INFO] %s\n' "$*"
}

if [[ "$#" -ne 1 ]]; then
    fail "Usage: $0 /path/to/dockstation-repka-pi5-arm64.tar.gz"
fi

ARCHIVE="$1"
[[ -f "$ARCHIVE" ]] || fail "Bundle archive not found: $ARCHIVE"

for command_name in tar sha256sum file ldd; do
    command -v "$command_name" >/dev/null 2>&1 || fail "Required verification tool missing: $command_name"
done

TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT

# Validate member names before extraction. This bundle is deliberately limited
# to two executables, two unit files, one config example and three metadata files.
while IFS= read -r member; do
    member="${member#./}"
    member="${member%/}"
    [[ -n "$member" && "$member" != "." ]] || continue
    [[ "$member" != /* ]] || fail "Absolute archive path is not allowed: $member"
    case "/$member/" in
        */../*) fail "Parent traversal path is not allowed: $member" ;;
    esac
    case "$member" in
        BUNDLE-MANIFEST.txt|RUNTIME-DEPENDENCIES.txt|SHA256SUMS|\
        usr|usr/local|usr/local/bin|usr/local/lib|usr/local/lib/systemd|usr/local/lib/systemd/system|usr/local/share|usr/local/share/dockstation|\
        usr/local/bin/dockstation_linux_bootstrap|usr/local/bin/dockstation-credential-provision|\
        usr/local/lib/systemd/system/dockstation-gateway.service|usr/local/lib/systemd/system/dockstation-gateway-uart@.service|\
        usr/local/share/dockstation/gateway.conf.example)
            ;;
        *)
            fail "Unexpected file in ARM64 bundle: $member"
            ;;
    esac
done < <(tar -tzf "$ARCHIVE")

# Reject links in archive entries: extraction must not follow a link outside
# the controlled package layout.
while IFS= read -r entry; do
    entry_type="${entry:0:1}"
    [[ "$entry_type" != "l" && "$entry_type" != "h" ]] ||
        fail "Links are not allowed in bundle archive: $entry"
done < <(tar -tvzf "$ARCHIVE")

tar --no-same-owner -xzf "$ARCHIVE" -C "$TEMP_ROOT"
ROOT="$TEMP_ROOT"

[[ -f "$ROOT/BUNDLE-MANIFEST.txt" ]] || fail "Manifest is missing."
[[ -f "$ROOT/RUNTIME-DEPENDENCIES.txt" ]] || fail "Runtime dependency report is missing."
[[ -f "$ROOT/SHA256SUMS" ]] || fail "SHA256SUMS is missing."
grep -Fqx 'Bundle format: dockstation-repka-pi5-arm64-v1' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Unsupported bundle format."
grep -Fqx 'Architecture: aarch64' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Bundle architecture is not aarch64."

(
    cd "$ROOT"
    sha256sum --check --strict SHA256SUMS
) || fail "One or more bundle file checksums do not match."

BINARIES=(
    "$ROOT/usr/local/bin/dockstation_linux_bootstrap"
    "$ROOT/usr/local/bin/dockstation-credential-provision"
)
for binary in "${BINARIES[@]}"; do
    [[ -x "$binary" ]] || fail "Bundle binary missing or not executable: $binary"
    description="$(file -b "$binary")"
    printf '%s\n' "$description" | grep -Eiq 'ELF 64-bit.*(ARM aarch64|AArch64)' ||
        fail "Binary is not a 64-bit AArch64 ELF: $binary ($description)"
    dependency_output="$(ldd "$binary" 2>&1)" ||
        fail "ldd failed for $binary: $dependency_output"
    if grep -Fq 'not found' <<< "$dependency_output"; then
        fail "Unresolved runtime dependency in $binary: $dependency_output"
    fi
done

for expected_file in \
    "$ROOT/usr/local/lib/systemd/system/dockstation-gateway.service" \
    "$ROOT/usr/local/lib/systemd/system/dockstation-gateway-uart@.service" \
    "$ROOT/usr/local/share/dockstation/gateway.conf.example"; do
    [[ -f "$expected_file" ]] || fail "Bundle file missing: $expected_file"
done

info "SHA-256 checksums match."
info "Both executables are 64-bit AArch64 ELF files."
info "All dynamic runtime dependencies resolve on this ARM64 build runner."
info "Bundle contains the Gateway binary, credential provisioner, both systemd units and config example."
printf 'PASS: DockStation ARM64 deployment bundle verification completed.\n'
