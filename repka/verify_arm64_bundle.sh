#!/usr/bin/env bash
set -euo pipefail

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '[INFO] %s\n' "$*"; }

[[ "$#" -eq 1 ]] || fail "Usage: $0 /path/to/dockstation-repka-pi5-arm64.tar.gz"
ARCHIVE="$1"
[[ -f "$ARCHIVE" ]] || fail "Bundle archive not found: $ARCHIVE"

for tool in tar sha256sum ldd mktemp; do
    command -v "$tool" >/dev/null 2>&1 || fail "Required verification tool missing: $tool"
done

TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT

# Validate member names before extraction. Only the release package layout is accepted.
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

# Reject symlinks/hardlinks so extraction cannot redirect writes outside the bundle root.
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
(cd "$ROOT" && sha256sum --check --strict SHA256SUMS) ||
    fail "One or more bundle file checksums do not match."

BINARIES=(
    "$ROOT/usr/local/bin/dockstation_linux_bootstrap"
    "$ROOT/usr/local/bin/dockstation-credential-provision"
)
for binary in "${BINARIES[@]}"; do
    [[ -x "$binary" ]] || fail "Bundle binary missing or not executable: $binary"

    # 'file' and 'readelf' are optional convenience checks. The native ldd check
    # below is mandatory and will reject an incompatible executable/loader.
    if command -v file >/dev/null 2>&1; then
        description="$(file -b "$binary")"
        printf '%s\n' "$description" | grep -Eiq 'ELF 64-bit.*(ARM aarch64|AArch64)' ||
            fail "Binary is not a 64-bit AArch64 ELF: $binary ($description)"
    elif command -v readelf >/dev/null 2>&1; then
        machine="$(readelf -h "$binary" | awk -F: '/Machine:/ { sub(/^[[:space:]]+/, "", $2); print $2 }')"
        [[ "$machine" == "AArch64" ]] || fail "Binary ELF machine is not AArch64: $binary ($machine)"
    else
        info "file/readelf unavailable; native ldd check is the runtime compatibility gate."
    fi

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

info "SHA-256 checksums match; archive paths and link types are accepted."
info "Both executables passed architecture inspection where available and native ldd checks."
printf 'PASS: DockStation ARM64 deployment bundle verification completed.\n'
