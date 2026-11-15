#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '[INFO] %s\n' "$*"; }

[[ "$#" -eq 1 ]] || fail "Usage: $0 /path/to/dockstation-repka-pi5-arm64.tar.gz"
ARCHIVE="$1"
[[ -f "$ARCHIVE" && ! -L "$ARCHIVE" ]] || fail "Bundle archive missing, not a regular file, or is a symlink: $ARCHIVE"

for tool in tar sha256sum ldd mktemp; do
    command -v "$tool" >/dev/null 2>&1 || fail "Required verification tool missing: $tool"
done

TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf -- "$TEMP_ROOT"' EXIT

# Accept only the exact release layout. Duplicate paths are rejected because
# extraction order must never decide which content is later installed.
declare -A SEEN_MEMBERS=()
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
    # Only allow-listed constant path keys are used for duplicate tracking.
    [[ -z "${SEEN_MEMBERS[$member]+present}" ]] ||
        fail "Duplicate archive member is not allowed: $member"
    SEEN_MEMBERS["$member"]=1
done < <(tar -tzf "$ARCHIVE")

# Only ordinary files and directories are valid. This rejects links, FIFOs,
# sockets, block/character devices and unknown entry types before extraction.
while IFS= read -r entry; do
    entry_type="${entry:0:1}"
    entry_path="${entry##* }"
    entry_path="${entry_path#./}"
    entry_path="${entry_path%/}"
    [[ -n "$entry_path" && "$entry_path" != "." ]] || continue
    case "$entry_path" in
        usr|usr/local|usr/local/bin|usr/local/lib|usr/local/lib/systemd|usr/local/lib/systemd/system|usr/local/share|usr/local/share/dockstation)
            expected_type="d"
            ;;
        BUNDLE-MANIFEST.txt|RUNTIME-DEPENDENCIES.txt|SHA256SUMS|\
        usr/local/bin/dockstation_linux_bootstrap|usr/local/bin/dockstation-credential-provision|\
        usr/local/lib/systemd/system/dockstation-gateway.service|usr/local/lib/systemd/system/dockstation-gateway-uart@.service|\
        usr/local/share/dockstation/gateway.conf.example)
            expected_type="-"
            ;;
        *) fail "Unable to classify archive entry type/path: $entry" ;;
    esac
    [[ "$entry_type" == "$expected_type" ]] ||
        fail "Archive member has an unsupported or mismatched type: $entry"
done < <(tar -tvzf "$ARCHIVE")

tar --no-same-owner --no-same-permissions -xzf "$ARCHIVE" -C "$TEMP_ROOT"
ROOT="$TEMP_ROOT"
[[ -f "$ROOT/BUNDLE-MANIFEST.txt" ]] || fail "Manifest is missing."
[[ -f "$ROOT/RUNTIME-DEPENDENCIES.txt" ]] || fail "Runtime dependency report is missing."
[[ -f "$ROOT/SHA256SUMS" ]] || fail "SHA256SUMS is missing."
grep -Fqx 'Bundle format: dockstation-repka-pi5-arm64-v1' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Unsupported bundle format."
grep -Fqx 'Architecture: aarch64' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Bundle architecture is not aarch64."
for required_line in \
    'Build type: Release' \
    'SQLite persistence: disabled' \
    'Install prefix: /usr/local' \
    'ABI ceilings: GLIBC<=2.34, GLIBCXX<=3.4.32, CXXABI<=1.3.9, OPENSSL<=3.0.0'; do
    grep -Fqx "$required_line" "$ROOT/BUNDLE-MANIFEST.txt" ||
        fail "Bundle manifest is missing or changes required policy: $required_line"
done
SOURCE_COMMIT_VALUE="$(sed -n 's/^Source commit: //p' "$ROOT/BUNDLE-MANIFEST.txt")"
[[ "$SOURCE_COMMIT_VALUE" =~ ^[[:xdigit:]]{40}$ ]] ||
    fail "Bundle manifest must contain one 40-hex Git source commit."
[[ "$(sed -n 's/^Source date epoch: //p' "$ROOT/BUNDLE-MANIFEST.txt")" =~ ^[0-9]+$ ]] ||
    fail "Bundle manifest must contain a numeric Source date epoch."
grep -Eq '^Build OS: .+' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Bundle manifest is missing Build OS."
grep -Eq '^Build kernel: .+' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Bundle manifest is missing Build kernel."
grep -Eq '^Compiler: .+' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Bundle manifest is missing Compiler."
grep -Eq '^CMake: .+' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Bundle manifest is missing CMake."
grep -Eq '^OpenSSL build version: 3\.' "$ROOT/BUNDLE-MANIFEST.txt" ||
    fail "Bundle manifest must record an OpenSSL 3 build version."
if [[ -n "${DOCKSTATION_EXPECTED_SOURCE_COMMIT:-}" ]]; then
    [[ "$SOURCE_COMMIT_VALUE" == "$DOCKSTATION_EXPECTED_SOURCE_COMMIT" ]] ||
        fail "Bundle source commit does not match the CI checkout."
fi
# Validate every checksum path before sha256sum is allowed to open it.
EXPECTED_CHECKSUM_NAMES=(
    ./BUNDLE-MANIFEST.txt
    ./RUNTIME-DEPENDENCIES.txt
    ./usr/local/bin/dockstation_linux_bootstrap
    ./usr/local/bin/dockstation-credential-provision
    ./usr/local/lib/systemd/system/dockstation-gateway.service
    ./usr/local/lib/systemd/system/dockstation-gateway-uart@.service
    ./usr/local/share/dockstation/gateway.conf.example
)
mapfile -t CHECKSUM_LINES < "$ROOT/SHA256SUMS"
(( ${#CHECKSUM_LINES[@]} == ${#EXPECTED_CHECKSUM_NAMES[@]} )) ||
    fail "SHA256SUMS must contain exactly ${#EXPECTED_CHECKSUM_NAMES[@]} payload entries."
ACTUAL_CHECKSUM_NAMES=""
for checksum_line in "${CHECKSUM_LINES[@]}"; do
    [[ "${checksum_line:0:64}" =~ ^[[:xdigit:]]{64}$ && "${checksum_line:64:2}" == "  " ]] ||
        fail "Malformed checksum line in SHA256SUMS."
    ACTUAL_CHECKSUM_NAMES+="${checksum_line:66}"$'\n'
done
EXPECTED_NAMES_SORTED="$(printf '%s\n' "${EXPECTED_CHECKSUM_NAMES[@]}" | LC_ALL=C sort)"
ACTUAL_NAMES_SORTED="$(printf '%s' "$ACTUAL_CHECKSUM_NAMES" | LC_ALL=C sort)"
[[ "$ACTUAL_NAMES_SORTED" == "$EXPECTED_NAMES_SORTED" ]] ||
    fail "SHA256SUMS does not cover the exact expected bundle payload."

(cd "$ROOT" && sha256sum --check --strict SHA256SUMS) ||
    fail "One or more bundle file checksums do not match."

# The dependency report is a stable SONAME inventory, not ldd's ASLR-dependent
# address/path output. Every listed requirement must still resolve on the target.
DEPENDENCY_REPORT="$ROOT/RUNTIME-DEPENDENCIES.txt"
mapfile -t DEPENDENCY_LINES < "$DEPENDENCY_REPORT"
(( ${#DEPENDENCY_LINES[@]} >= 5 )) ||
    fail "Runtime dependency report is incomplete."
[[ "${DEPENDENCY_LINES[0]}" == "# DockStation runtime dependency report v1" ]] ||
    fail "Unsupported runtime dependency report format."
GATEWAY_SECTION="### usr/local/bin/dockstation_linux_bootstrap"
PROVISIONER_SECTION="### usr/local/bin/dockstation-credential-provision"
CURRENT_SECTION=""
SEEN_GATEWAY_SECTION=false
SEEN_PROVISIONER_SECTION=false
GATEWAY_BUILD_DEPENDENCIES=()
PROVISIONER_BUILD_DEPENDENCIES=()
PREVIOUS_DEPENDENCY=""
for dependency_line in "${DEPENDENCY_LINES[@]:1}"; do
    case "$dependency_line" in
        "$GATEWAY_SECTION")
            [[ "$SEEN_GATEWAY_SECTION" == false && "$SEEN_PROVISIONER_SECTION" == false ]] ||
                fail "Runtime dependency report has a duplicate or out-of-order Gateway section."
            SEEN_GATEWAY_SECTION=true
            CURRENT_SECTION=gateway
            PREVIOUS_DEPENDENCY=""
            ;;
        "$PROVISIONER_SECTION")
            [[ "$SEEN_GATEWAY_SECTION" == true && "$SEEN_PROVISIONER_SECTION" == false ]] ||
                fail "Runtime dependency report has a duplicate or out-of-order credential section."
            (( ${#GATEWAY_BUILD_DEPENDENCIES[@]} > 0 )) ||
                fail "Gateway runtime dependency list is empty."
            SEEN_PROVISIONER_SECTION=true
            CURRENT_SECTION=provisioner
            PREVIOUS_DEPENDENCY=""
            ;;
        *)
            [[ -n "$CURRENT_SECTION" ]] ||
                fail "Runtime dependency entry appears before a binary section."
            [[ "$dependency_line" =~ ^[A-Za-z0-9][A-Za-z0-9_.+-]*$ ]] ||
                fail "Malformed SONAME in runtime dependency report: $dependency_line"
            [[ -z "$PREVIOUS_DEPENDENCY" || "$dependency_line" > "$PREVIOUS_DEPENDENCY" ]] ||
                fail "Runtime dependency SONAMEs must be unique and sorted."
            PREVIOUS_DEPENDENCY="$dependency_line"
            if [[ "$CURRENT_SECTION" == gateway ]]; then
                GATEWAY_BUILD_DEPENDENCIES+=("$dependency_line")
            else
                PROVISIONER_BUILD_DEPENDENCIES+=("$dependency_line")
            fi
            ;;
    esac
done
[[ "$SEEN_GATEWAY_SECTION" == true && "$SEEN_PROVISIONER_SECTION" == true ]] ||
    fail "Runtime dependency report must contain both executable sections."
(( ${#GATEWAY_BUILD_DEPENDENCIES[@]} > 0 && ${#PROVISIONER_BUILD_DEPENDENCIES[@]} > 0 )) ||
    fail "Runtime dependency report has an empty executable section."

BINARIES=(
    "$ROOT/usr/local/bin/dockstation_linux_bootstrap"
    "$ROOT/usr/local/bin/dockstation-credential-provision"
)
for binary in "${BINARIES[@]}"; do
    [[ -f "$binary" && ! -L "$binary" && -x "$binary" ]] ||
        fail "Bundle binary missing, non-regular or not executable: $binary"

    # 'file' and 'readelf' are optional architecture-inspection tools. When
    # readelf is available, also enforce the selected ABI symbol-version ceilings.
    if command -v file >/dev/null 2>&1; then
        description="$(file -b "$binary")"
        printf '%s\n' "$description" | grep -Eiq 'ELF 64-bit.*(ARM aarch64|AArch64)' ||
            fail "Binary is not a 64-bit AArch64 ELF: $binary ($description)"
    fi

    if command -v readelf >/dev/null 2>&1; then
        machine="$(readelf -h "$binary" | awk -F: '/Machine:/ { sub(/^[[:space:]]+/, "", $2); print $2 }')"
        [[ "$machine" == "AArch64" ]] || fail "Binary ELF machine is not AArch64: $binary ($machine)"
        version_names="$(
            readelf --version-info "$binary" 2>/dev/null |
                grep -oE 'Name: (GLIBC|GLIBCXX|CXXABI|OPENSSL)_[0-9.]+' |
                awk '{ print $2 }' |
                sort -Vu || true
        )"

        for symbol_family in GLIBC GLIBCXX CXXABI OPENSSL; do
            case "$symbol_family" in
                GLIBC) ceiling="GLIBC_2.34" ;;
                GLIBCXX) ceiling="GLIBCXX_3.4.32" ;;
                CXXABI) ceiling="CXXABI_1.3.9" ;;
                OPENSSL) ceiling="OPENSSL_3.0.0" ;;
            esac
            highest="$(grep -E "^${symbol_family}_[0-9.]+" <<< "$version_names" | sort -V | tail -n 1 || true)"
            if [[ -n "$highest" ]]; then
                comparison="$(printf '%s\n%s\n' "$highest" "$ceiling" | sort -V | tail -n 1)"
                [[ "$comparison" == "$ceiling" ]] ||
                    fail "$binary requires $highest, above the selected ABI ceiling $ceiling."
                info "$binary: maximum $symbol_family requirement is $highest (ceiling $ceiling)."
            fi
        done
    else
        [[ "${DOCKSTATION_REQUIRE_ABI_AUDIT:-0}" != "1" ]] ||
            fail "readelf is required by DOCKSTATION_REQUIRE_ABI_AUDIT=1."
        info "readelf unavailable; symbol-version audit skipped. Native ldd remains mandatory."
    fi

    dependency_output="$(ldd "$binary" 2>&1)" ||
        fail "ldd failed for $binary: $dependency_output"
    if grep -Fq 'not found' <<< "$dependency_output"; then
        fail "Unresolved runtime dependency in $binary: $dependency_output"
    fi
    actual_dependency_names="$(
        awk '
            $1 == "linux-vdso.so.1" { next }
            $2 == "=>" { print $1; next }
            $1 ~ /^\// {
                name = $1
                sub(/^.*\//, "", name)
                print name
            }
        ' <<< "$dependency_output" | sort -u
    )" || fail "Could not parse ldd dependencies for $binary."
    [[ -n "$actual_dependency_names" ]] ||
        fail "ldd produced no shared-library SONAMEs for $binary."
    if [[ "${binary##*/}" == dockstation_linux_bootstrap ]]; then
        expected_dependency_names=("${GATEWAY_BUILD_DEPENDENCIES[@]}")
    else
        expected_dependency_names=("${PROVISIONER_BUILD_DEPENDENCIES[@]}")
    fi
    if [[ "${DOCKSTATION_REQUIRE_EXACT_RUNTIME_SONAMES:-0}" == "1" ]]; then
        expected_names_sorted="$(printf '%s\n' "${expected_dependency_names[@]}" | sort -u)"
        actual_names_sorted="$(printf '%s\\n' "$actual_dependency_names" | sort -u)"
        [[ "$actual_names_sorted" == "$expected_names_sorted" ]] ||
            fail "$binary runtime SONAME set differs from the build inventory."
    else
        for expected_dependency in "${expected_dependency_names[@]}"; do
            grep -Fqx -- "$expected_dependency" <<< "$actual_dependency_names" ||
                fail "$binary is missing a build-recorded runtime SONAME on this target: $expected_dependency"
        done
    fi
done

for expected_file in \
    "$ROOT/usr/local/lib/systemd/system/dockstation-gateway.service" \
    "$ROOT/usr/local/lib/systemd/system/dockstation-gateway-uart@.service" \
    "$ROOT/usr/local/share/dockstation/gateway.conf.example"; do
    [[ -f "$expected_file" && ! -L "$expected_file" ]] ||
        fail "Bundle file missing or not a regular file: $expected_file"
done

info "SHA-256 checksums match; archive paths and link types are accepted."
info "Both executables passed architecture inspection where available and native ldd checks."
printf 'PASS: DockStation ARM64 deployment bundle verification completed.\n'
