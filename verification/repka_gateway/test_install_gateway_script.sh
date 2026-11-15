#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/../.." && pwd)"
INSTALLER="$ROOT/repka/install_gateway.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
BIN="$TMP/bin"
STAGE_SOURCE="$TMP/source-stage"
STAGE_BUNDLE="$TMP/bundle-stage"
LOG="$TMP/commands.log"
mkdir -p "$BIN" "$STAGE_SOURCE" "$STAGE_BUNDLE"
: > "$LOG"

cat > "$BIN/source-preflight" <<'SH'
#!/usr/bin/env bash
echo source-preflight >> "$DOCKSTATION_TEST_LOG"
exit 1
SH
cat > "$BIN/runtime-preflight" <<'SH'
#!/usr/bin/env bash
echo runtime-preflight >> "$DOCKSTATION_TEST_LOG"
exit 1
SH
cat > "$BIN/cmake" <<'SH'
#!/usr/bin/env bash
echo "cmake $*" >> "$DOCKSTATION_TEST_LOG"
if [[ "$1" == --install ]]; then
    d="${DESTDIR:-}"
    mkdir -p "$d/usr/local/bin" "$d/usr/local/lib/systemd/system" "$d/usr/local/share/dockstation"
    printf '#!/bin/sh\nexit 0\n' > "$d/usr/local/bin/dockstation_linux_bootstrap"
    printf '#!/bin/sh\nexit 0\n' > "$d/usr/local/bin/dockstation-credential-provision"
    chmod +x "$d/usr/local/bin/"*
    printf '[Service]\nPrivateDevices=true\n' > "$d/usr/local/lib/systemd/system/dockstation-gateway.service"
    printf '[Service]\nPrivateDevices=false\nDevicePolicy=closed\nDeviceAllow=/dev/%%i rw\n' > "$d/usr/local/lib/systemd/system/dockstation-gateway-uart@.service"
    printf 'api_port=8080\nlog_stderr=true\n' > "$d/usr/local/share/dockstation/gateway.conf.example"
fi
SH
cat > "$BIN/systemd-analyze" <<'SH'
#!/usr/bin/env bash
echo "systemd-analyze $*" >> "$DOCKSTATION_TEST_LOG"
[[ "$1" == verify && -f "$2" && -f "$3" ]]
SH
cat > "$BIN/file" <<'SH'
#!/usr/bin/env bash
printf '%s\n' 'ELF 64-bit LSB pie executable, ARM aarch64, version 1 (SYSV)'
SH
cat > "$BIN/readelf" <<'SH'
#!/usr/bin/env bash
case "$1" in
    -h) printf '%s\n' '  Machine:                           AArch64' ;;
    --version-info)
        printf '%s\n' \
            '  Name: GLIBC_2.34' \
            "  Name: GLIBCXX_${FAKE_GLIBCXX_VERSION:-3.4.32}" \
            '  Name: CXXABI_1.3.9' \
            '  Name: OPENSSL_3.0.0'
        ;;
    *) exit 64 ;;
esac
SH
cat > "$BIN/ldd" <<'SH'
#!/usr/bin/env bash
if [[ "${1:-}" == --version ]]; then
    printf '%s\n' 'ldd (GNU libc) 2.39'
else
    printf '%s\n' 'libssl.so.3 => /lib/aarch64-linux-gnu/libssl.so.3'
fi
SH
chmod +x "$BIN/"*

export PATH="$BIN:/usr/bin:/bin"
export DOCKSTATION_PREFLIGHT_SCRIPT="$BIN/source-preflight"
export DOCKSTATION_RUNTIME_PREFLIGHT_SCRIPT="$BIN/runtime-preflight"
export DOCKSTATION_CMAKE_COMMAND="$BIN/cmake"
export DOCKSTATION_SYSTEMD_ANALYZE_COMMAND="$BIN/systemd-analyze"
export DOCKSTATION_INSTALL_ROOT="$STAGE_SOURCE"
export DOCKSTATION_BUILD_DIR="$TMP/build"
export DOCKSTATION_BUILD_JOBS=2
export DOCKSTATION_TEST_LOG="$LOG"
export DOCKSTATION_REQUIRE_EXACT_RUNTIME_SONAMES=1
export DOCKSTATION_EXPECTED_SOURCE_COMMIT=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb

# Source installation staging remains the development fallback.
bash "$INSTALLER"
[[ -x "$STAGE_SOURCE/usr/local/bin/dockstation_linux_bootstrap" ]]
[[ -f "$STAGE_SOURCE/etc/dockstation/gateway.conf" ]]
grep -Fqx 'api_port=8080' "$STAGE_SOURCE/etc/dockstation/gateway.conf"
grep -Fq -- '-DDOCKSTATION_BUILD_SQLITE_PERSISTENCE=OFF' "$LOG"
printf 'operator_setting=preserve-me\n' > "$STAGE_SOURCE/etc/dockstation/gateway.conf"
bash "$INSTALLER"
grep -Fqx 'operator_setting=preserve-me' "$STAGE_SOURCE/etc/dockstation/gateway.conf"

# Synthetic ARM64 bundle exercises archive validation and the no-CMake path.
BROOT="$TMP/bundle-root"
mkdir -p "$BROOT/usr/local/bin" "$BROOT/usr/local/lib/systemd/system" "$BROOT/usr/local/share/dockstation"
for name in dockstation_linux_bootstrap dockstation-credential-provision; do
    printf '#!/bin/sh\nexit 0\n' > "$BROOT/usr/local/bin/$name"
    chmod +x "$BROOT/usr/local/bin/$name"
done
printf '[Service]\nPrivateDevices=true\n' > "$BROOT/usr/local/lib/systemd/system/dockstation-gateway.service"
printf '[Service]\nPrivateDevices=false\nDevicePolicy=closed\nDeviceAllow=/dev/%%i rw\n' > "$BROOT/usr/local/lib/systemd/system/dockstation-gateway-uart@.service"
printf 'api_port=8080\nlog_stderr=true\n' > "$BROOT/usr/local/share/dockstation/gateway.conf.example"
printf '%s\n' \
    'Bundle format: dockstation-repka-pi5-arm64-v1' \
    'Architecture: aarch64' \
    'Build OS: Synthetic test OS' \
    'Build kernel: test-kernel' \
    'Compiler: c++ synthetic' \
    'CMake: cmake version 3.30.0' \
    'OpenSSL build version: 3.0.0' \
    'Source commit: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' \
    'Source date epoch: 1794760000' \
    'Build type: Release' \
    'SQLite persistence: disabled' \
    'Install prefix: /usr/local' \
    'ABI ceilings: GLIBC<=2.34, GLIBCXX<=3.4.32, CXXABI<=1.3.9, OPENSSL<=3.0.0' \
    > "$BROOT/BUNDLE-MANIFEST.txt"
{
    printf '%s\n' '# DockStation runtime dependency report v1'
    printf '%s\n' '### usr/local/bin/dockstation_linux_bootstrap' 'libssl.so.3'
    printf '%s\n' '### usr/local/bin/dockstation-credential-provision' 'libssl.so.3'
} > "$BROOT/RUNTIME-DEPENDENCIES.txt"
(
    cd "$BROOT"
    find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)
BUNDLE="$TMP/dockstation-repka-pi5-arm64.tar.gz"
tar -czf "$BUNDLE" -C "$BROOT" .
(cd "$TMP" && sha256sum "$(basename "$BUNDLE")" > "$(basename "$BUNDLE").sha256")

# Bundle installation must not invoke CMake at all.
: > "$LOG"
export DOCKSTATION_INSTALL_ROOT="$STAGE_BUNDLE"
export DOCKSTATION_CMAKE_COMMAND="$BIN/cmake-that-must-not-run"
bash "$INSTALLER" --bundle "$BUNDLE"
[[ -x "$STAGE_BUNDLE/usr/local/bin/dockstation_linux_bootstrap" ]]
[[ -x "$STAGE_BUNDLE/usr/local/bin/dockstation-credential-provision" ]]
[[ -f "$STAGE_BUNDLE/usr/local/lib/systemd/system/dockstation-gateway.service" ]]
[[ -f "$STAGE_BUNDLE/usr/local/lib/systemd/system/dockstation-gateway-uart@.service" ]]
[[ -f "$STAGE_BUNDLE/etc/dockstation/gateway.conf" ]]
grep -Fqx 'api_port=8080' "$STAGE_BUNDLE/etc/dockstation/gateway.conf"
if grep -Fq 'cmake ' "$LOG"; then
    echo "FAIL: bundle mode unexpectedly invoked CMake." >&2
    exit 1
fi
grep -Fq 'runtime-preflight' "$LOG"

# A newer libstdc++ symbol requirement must fail the documented ABI floor.
if FAKE_GLIBCXX_VERSION=3.4.33 bash "$INSTALLER" --bundle "$BUNDLE" >/dev/null 2>&1; then
    echo "FAIL: bundle requiring GLIBCXX_3.4.33 above the ceiling was accepted." >&2
    exit 1
fi

# Re-signing the outer archive must not hide a modified internal member.
BADROOT="$TMP/bad-root"
cp -a "$BROOT" "$BADROOT"
printf 'tampered\n' >> "$BADROOT/usr/local/share/dockstation/gateway.conf.example"
cp "$BROOT/SHA256SUMS" "$BADROOT/SHA256SUMS"
BAD_BUNDLE="$TMP/bad-bundle.tar.gz"
tar -czf "$BAD_BUNDLE" -C "$BADROOT" .
(cd "$TMP" && sha256sum "$(basename "$BAD_BUNDLE")" > "$(basename "$BAD_BUNDLE").sha256")
if bash "$INSTALLER" --bundle "$BAD_BUNDLE" >/dev/null 2>&1; then
    echo "FAIL: bundle with broken internal SHA256 was accepted." >&2
    exit 1
fi

if grep -Eiq 'apt(-get)? (update|upgrade|install)|do-release-upgrade' "$INSTALLER"; then
    echo "FAIL: installer contains OS package/update commands." >&2
    exit 1
fi
printf 'PASS: source staging, bundle staging without CMake, config preservation and archive integrity.\n'
