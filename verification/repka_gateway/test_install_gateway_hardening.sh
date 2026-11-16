#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT
BIN="$TMP/bin"
mkdir -p "$BIN"

cat > "$BIN/preflight" <<'SH'
#!/usr/bin/env bash
exit 1
SH
cat > "$BIN/systemd-analyze" <<'SH'
#!/usr/bin/env bash
[[ "$1" == verify ]]
SH
cat > "$BIN/cmake" <<'SH'
#!/usr/bin/env bash
if [[ "$1" == --install ]]; then
 d="${DESTDIR:-}"
 mkdir -p "$d/usr/local/bin" "$d/usr/local/lib/systemd/system" "$d/usr/local/share/dockstation"
 printf '#!/bin/sh\nexit 0\n' > "$d/usr/local/bin/dockstation_linux_bootstrap"
 printf '#!/bin/sh\nexit 0\n' > "$d/usr/local/bin/dockstation-credential-provision"
 printf '#!/bin/sh\nexit 0\n' > "$d/usr/local/bin/dockstation-registry-provision"
 chmod +x "$d/usr/local/bin/"*
 printf '[Service]\n' > "$d/usr/local/lib/systemd/system/dockstation-gateway.service"
 printf '[Service]\n' > "$d/usr/local/lib/systemd/system/dockstation-gateway-uart@.service"
 printf 'api_port=8080\n' > "$d/usr/local/share/dockstation/gateway.conf.example"
fi
SH
cat > "$BIN/file" <<'SH'
#!/usr/bin/env bash
printf 'ELF 64-bit LSB pie executable, ARM aarch64, version 1 (SYSV)\n'
SH
cat > "$BIN/readelf" <<'SH'
#!/usr/bin/env bash
case "$1" in
 -h) echo '  Machine: AArch64' ;;
 --version-info) printf ' Name: GLIBC_2.34\n Name: GLIBCXX_3.4.32\n Name: CXXABI_1.3.9\n Name: OPENSSL_3.0.0\n' ;;
 *) exit 64 ;;
esac
SH
cat > "$BIN/ldd" <<'SH'
#!/usr/bin/env bash
printf 'libcrypto.so.3 => /lib/aarch64-linux-gnu/libcrypto.so.3\n'
case "${1##*/}" in
 dockstation_linux_bootstrap|dockstation-registry-provision)
  printf 'libsqlite3.so.0 => /lib/aarch64-linux-gnu/libsqlite3.so.0\n'
  ;;
esac
printf 'libssl.so.3 => /lib/aarch64-linux-gnu/libssl.so.3\n'
SH
chmod +x "$BIN/"*
export PATH="$BIN:/usr/bin:/bin"
export DOCKSTATION_REQUIRE_EXACT_RUNTIME_SONAMES=1
export DOCKSTATION_EXPECTED_SOURCE_COMMIT=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
export DOCKSTATION_PREFLIGHT_SCRIPT="$BIN/preflight"
export DOCKSTATION_RUNTIME_PREFLIGHT_SCRIPT="$BIN/preflight"
export DOCKSTATION_SYSTEMD_ANALYZE_COMMAND="$BIN/systemd-analyze"

# Reject the empty bundle argument without falling back to source mode.
if bash "$ROOT/repka/install_gateway.sh" --bundle "" >/dev/null 2>&1; then
 echo 'FAIL: empty --bundle accepted' >&2; exit 1
fi

# Source staging remains viable and creates/preserves the initial config.
cat > "$BIN/cmake" <<'SH'
#!/usr/bin/env bash
if [[ "$1" == --install ]]; then
 d="${DESTDIR:-}"
 mkdir -p "$d/usr/local/bin" "$d/usr/local/lib/systemd/system" "$d/usr/local/share/dockstation"
 printf '#!/bin/sh\nexit 0\n' > "$d/usr/local/bin/dockstation_linux_bootstrap"
 printf '#!/bin/sh\nexit 0\n' > "$d/usr/local/bin/dockstation-credential-provision"
 printf '#!/bin/sh\nexit 0\n' > "$d/usr/local/bin/dockstation-registry-provision"
 chmod +x "$d/usr/local/bin/"*
 printf '[Service]\n' > "$d/usr/local/lib/systemd/system/dockstation-gateway.service"
 printf '[Service]\n' > "$d/usr/local/lib/systemd/system/dockstation-gateway-uart@.service"
 printf 'api_port=8080\n' > "$d/usr/local/share/dockstation/gateway.conf.example"
fi
SH
chmod +x "$BIN/cmake"
export DOCKSTATION_CMAKE_COMMAND="$BIN/cmake"
STAGE="$TMP/source-stage"
DOCKSTATION_INSTALL_ROOT="$STAGE" bash "$ROOT/repka/install_gateway.sh" >/dev/null
grep -Fqx 'api_port=8080' "$STAGE/etc/dockstation/gateway.conf"
printf 'operator_setting=source-preserve\n' >> "$STAGE/etc/dockstation/gateway.conf"
DOCKSTATION_INSTALL_ROOT="$STAGE" bash "$ROOT/repka/install_gateway.sh" >/dev/null
grep -Fqx 'operator_setting=source-preserve' "$STAGE/etc/dockstation/gateway.conf"

# Build a valid synthetic archive for the verifier and bundle installer.
BROOT="$TMP/bundle-root"
mkdir -p "$BROOT/usr/local/bin" "$BROOT/usr/local/lib/systemd/system" "$BROOT/usr/local/share/dockstation"
for name in dockstation_linux_bootstrap dockstation-credential-provision dockstation-registry-provision; do
 printf '#!/bin/sh\nexit 0\n' > "$BROOT/usr/local/bin/$name"; chmod +x "$BROOT/usr/local/bin/$name"
done
printf '[Service]\n' > "$BROOT/usr/local/lib/systemd/system/dockstation-gateway.service"
printf '[Service]\n' > "$BROOT/usr/local/lib/systemd/system/dockstation-gateway-uart@.service"
printf 'api_port=8080\n' > "$BROOT/usr/local/share/dockstation/gateway.conf.example"
printf '%s\n' \
 'Bundle format: dockstation-repka-pi5-arm64-v2' \
 'Architecture: aarch64' \
 'Build OS: Synthetic test OS' \
 'Build kernel: test-kernel' \
 'Compiler: c++ synthetic' \
 'CMake: cmake version 3.30.0' \
 'OpenSSL build version: 3.0.0' \
 'Source commit: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' \
 'Source date epoch: 1794760000' \
 'Build type: Release' \
 'SQLite persistence: enabled' \
 'Install prefix: /usr/local' \
 'ABI ceilings: GLIBC<=2.34, GLIBCXX<=3.4.32, CXXABI<=1.3.9, OPENSSL<=3.0.0' \
 > "$BROOT/BUNDLE-MANIFEST.txt"
{
 printf '%s\n' '# DockStation runtime dependency report v2'
 printf '%s\n' '### usr/local/bin/dockstation_linux_bootstrap'
 printf '%s\n' 'libcrypto.so.3' 'libsqlite3.so.0' 'libssl.so.3'
 printf '%s\n' '### usr/local/bin/dockstation-credential-provision'
 printf '%s\n' 'libcrypto.so.3' 'libssl.so.3'
 printf '%s\n' '### usr/local/bin/dockstation-registry-provision'
 printf '%s\n' 'libcrypto.so.3' 'libsqlite3.so.0' 'libssl.so.3'
} > "$BROOT/RUNTIME-DEPENDENCIES.txt"
(cd "$BROOT" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
BUNDLE="$TMP/dockstation-repka-pi5-arm64.tar.gz"
tar -czf "$BUNDLE" -C "$BROOT" .
(cd "$TMP" && sha256sum "$(basename "$BUNDLE")" > "$(basename "$BUNDLE").sha256")
bash "$ROOT/repka/verify_arm64_bundle.sh" "$BUNDLE" >/dev/null
if DOCKSTATION_EXPECTED_SOURCE_COMMIT=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb \
    bash "$ROOT/repka/verify_arm64_bundle.sh" "$BUNDLE" >/dev/null 2>&1; then
 echo 'FAIL: bundle source commit mismatch accepted' >&2; exit 1
fi

# A dependency report that omits a library observed by ldd must not pass CI.
cp -a "$BROOT" "$TMP/incomplete-runtime-report-root"
awk '
  $0 == "### usr/local/bin/dockstation-credential-provision" { provisioner=1; print; next }
  provisioner && $0 == "libcrypto.so.3" { next }
  { print }
' "$TMP/incomplete-runtime-report-root/RUNTIME-DEPENDENCIES.txt" > "$TMP/incomplete-runtime-report-root/RUNTIME-DEPENDENCIES.new"
mv "$TMP/incomplete-runtime-report-root/RUNTIME-DEPENDENCIES.new" "$TMP/incomplete-runtime-report-root/RUNTIME-DEPENDENCIES.txt"
(cd "$TMP/incomplete-runtime-report-root" && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -czf "$TMP/incomplete-runtime-report.tar.gz" -C "$TMP/incomplete-runtime-report-root" .
if bash "$ROOT/repka/verify_arm64_bundle.sh" "$TMP/incomplete-runtime-report.tar.gz" >/dev/null 2>&1; then
 echo 'FAIL: incomplete runtime SONAME report accepted' >&2; exit 1
fi
STAGE2="$TMP/bundle-stage"
DOCKSTATION_INSTALL_ROOT="$STAGE2" DOCKSTATION_CMAKE_COMMAND=/bin/false \
 bash "$ROOT/repka/install_gateway.sh" --bundle "$BUNDLE" >/dev/null
grep -Fqx 'api_port=8080' "$STAGE2/etc/dockstation/gateway.conf"
[[ -x "$STAGE2/usr/local/bin/dockstation-registry-provision" ]]
printf 'operator_setting=bundle-preserve\n' >> "$STAGE2/etc/dockstation/gateway.conf"
DOCKSTATION_INSTALL_ROOT="$STAGE2" DOCKSTATION_CMAKE_COMMAND=/bin/false \
 bash "$ROOT/repka/install_gateway.sh" --bundle "$BUNDLE" >/dev/null
grep -Fqx 'operator_setting=bundle-preserve' "$STAGE2/etc/dockstation/gateway.conf"

# The sidecar filename must bind to the passed archive.
cp "$BUNDLE" "$TMP/alternate.tar.gz"
printf 'not-the-bundle\n' >> "$TMP/alternate.tar.gz"
sha256sum "$TMP/alternate.tar.gz" > "$BUNDLE.sha256"
if DOCKSTATION_INSTALL_ROOT="$TMP/mismatch-stage" bash "$ROOT/repka/install_gateway.sh" --bundle "$BUNDLE" >/dev/null 2>&1; then
 echo 'FAIL: mismatched sidecar filename accepted' >&2; exit 1
fi
[[ ! -e "$TMP/mismatch-stage/usr/local/bin/dockstation_linux_bootstrap" ]]

# Multiple sidecar rows are ambiguous and must be rejected.
(cd "$TMP" && sha256sum "$(basename "$BUNDLE")" > "$(basename "$BUNDLE").sha256")
checksum_line="$(cat "$BUNDLE.sha256")"
printf '%s\n%s\n' "$checksum_line" "$checksum_line" > "$BUNDLE.sha256"
if DOCKSTATION_INSTALL_ROOT="$TMP/multi-sidecar-stage" bash "$ROOT/repka/install_gateway.sh" --bundle "$BUNDLE" >/dev/null 2>&1; then
 echo 'FAIL: multi-line sidecar accepted' >&2; exit 1
fi
[[ ! -e "$TMP/multi-sidecar-stage/usr/local/bin/dockstation_linux_bootstrap" ]]
(cd "$TMP" && sha256sum "$(basename "$BUNDLE")" > "$(basename "$BUNDLE").sha256")

# Symlinked destinations and parents must fail before any installation writes.
mkdir -p "$TMP/config-symlink-stage/etc"
ln -s "$TMP/external" "$TMP/config-symlink-stage/etc/dockstation"
if DOCKSTATION_INSTALL_ROOT="$TMP/config-symlink-stage" bash "$ROOT/repka/install_gateway.sh" --bundle "$BUNDLE" >/dev/null 2>&1; then
 echo 'FAIL: symlinked config directory accepted' >&2; exit 1
fi
[[ ! -e "$TMP/config-symlink-stage/usr/local/bin/dockstation_linux_bootstrap" ]]

mkdir -p "$TMP/etc-parent-symlink-stage"
ln -s "$TMP/external" "$TMP/etc-parent-symlink-stage/etc"
if DOCKSTATION_INSTALL_ROOT="$TMP/etc-parent-symlink-stage" bash "$ROOT/repka/install_gateway.sh" --bundle "$BUNDLE" >/dev/null 2>&1; then
 echo 'FAIL: symlinked staging /etc parent accepted' >&2; exit 1
fi
[[ ! -e "$TMP/etc-parent-symlink-stage/usr/local/bin/dockstation_linux_bootstrap" ]]

mkdir -p "$TMP/file-symlink-stage/usr/local/bin"
ln -s "$TMP/external-binary" "$TMP/file-symlink-stage/usr/local/bin/dockstation_linux_bootstrap"
if DOCKSTATION_INSTALL_ROOT="$TMP/file-symlink-stage" bash "$ROOT/repka/install_gateway.sh" --bundle "$BUNDLE" >/dev/null 2>&1; then
 echo 'FAIL: symlinked binary destination accepted' >&2; exit 1
fi
[[ ! -e "$TMP/file-symlink-stage/usr/local/lib/systemd/system/dockstation-gateway.service" ]]

# Change the caller's archive during verification; extraction must still use the snapshot.
REPLACEMENT="$TMP/replacement.tar.gz"
cp "$BUNDLE" "$REPLACEMENT"
cp -a "$BROOT" "$TMP/replacement-root"
printf '# mutation\n' >> "$TMP/replacement-root/usr/local/share/dockstation/gateway.conf.example"
(cd "$TMP/replacement-root" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -czf "$REPLACEMENT" -C "$TMP/replacement-root" .
cat > "$BIN/mutating-verifier" <<'SH'
#!/usr/bin/env bash
cp -- "$DOCKSTATION_REPLACEMENT" "$DOCKSTATION_SOURCE_ARCHIVE"
SH
chmod +x "$BIN/mutating-verifier"
(cd "$TMP" && sha256sum "$(basename "$BUNDLE")" > "$(basename "$BUNDLE").sha256")
STAGE3="$TMP/snapshot-stage"
DOCKSTATION_INSTALL_ROOT="$STAGE3" \
DOCKSTATION_BUNDLE_VERIFIER="$BIN/mutating-verifier" \
DOCKSTATION_REPLACEMENT="$REPLACEMENT" \
DOCKSTATION_SOURCE_ARCHIVE="$BUNDLE" \
 bash "$ROOT/repka/install_gateway.sh" --bundle "$BUNDLE" >/dev/null
grep -Fqx 'api_port=8080' "$STAGE3/usr/local/share/dockstation/gateway.conf.example"
cmp -s "$BUNDLE" "$REPLACEMENT" || { echo 'FAIL: source archive was not replaced by the verifier stub' >&2; exit 1; }

# Duplicate archive names, incomplete hashes and malicious checksum paths are rejected.
tar -cf "$TMP/duplicate.tar" -C "$BROOT" .
tar -rf "$TMP/duplicate.tar" -C "$BROOT" ./usr/local/share/dockstation/gateway.conf.example
gzip -c "$TMP/duplicate.tar" > "$TMP/duplicate.tar.gz"
if bash "$ROOT/repka/verify_arm64_bundle.sh" "$TMP/duplicate.tar.gz" >/dev/null 2>&1; then
 echo 'FAIL: duplicate archive member accepted' >&2; exit 1
fi

cp -a "$BROOT" "$TMP/incomplete-checksum-root"
grep -v 'gateway.conf.example' "$TMP/incomplete-checksum-root/SHA256SUMS" > "$TMP/incomplete-checksum-root/SHA256SUMS.new"
mv "$TMP/incomplete-checksum-root/SHA256SUMS.new" "$TMP/incomplete-checksum-root/SHA256SUMS"
tar -czf "$TMP/incomplete-checksum.tar.gz" -C "$TMP/incomplete-checksum-root" .
if bash "$ROOT/repka/verify_arm64_bundle.sh" "$TMP/incomplete-checksum.tar.gz" >/dev/null 2>&1; then
 echo 'FAIL: incomplete checksum manifest accepted' >&2; exit 1
fi

cp -a "$BROOT" "$TMP/external-checksum-root"
printf 'outside bundle\n' > "$TMP/outside-checksum-target"
external_checksum_line="$(sha256sum "$TMP/outside-checksum-target")"
grep -v 'gateway.conf.example' "$TMP/external-checksum-root/SHA256SUMS" > "$TMP/external-checksum-root/SHA256SUMS.new"
printf '%s\n' "$external_checksum_line" >> "$TMP/external-checksum-root/SHA256SUMS.new"
mv "$TMP/external-checksum-root/SHA256SUMS.new" "$TMP/external-checksum-root/SHA256SUMS"
tar -czf "$TMP/external-checksum.tar.gz" -C "$TMP/external-checksum-root" .
REAL_SHA256SUM="$(command -v sha256sum)"
cat > "$BIN/sha256sum-guard" <<SH
#!/usr/bin/env bash
for arg in "\$@"; do
 if [[ "\$arg" == --check ]]; then
  : > "\$DOCKSTATION_SHA256_CHECK_MARKER"
  exit 99
 fi
done
exec "$REAL_SHA256SUM" "\$@"
SH
chmod +x "$BIN/sha256sum-guard"
mkdir -p "$TMP/guard-bin"
cp "$BIN/sha256sum-guard" "$TMP/guard-bin/sha256sum"
if DOCKSTATION_SHA256_CHECK_MARKER="$TMP/sha256-check-was-called" PATH="$TMP/guard-bin:$PATH" \
    bash "$ROOT/repka/verify_arm64_bundle.sh" "$TMP/external-checksum.tar.gz" >/dev/null 2>&1; then
 echo 'FAIL: external checksum path accepted' >&2; exit 1
fi
[[ ! -e "$TMP/sha256-check-was-called" ]] || {
 echo 'FAIL: verifier hashed an unchecked external path' >&2; exit 1;
}

# A FIFO at an otherwise allow-listed archive path is rejected.
cp -a "$BROOT" "$TMP/fifo-root"
rm "$TMP/fifo-root/usr/local/share/dockstation/gateway.conf.example"
mkfifo "$TMP/fifo-root/usr/local/share/dockstation/gateway.conf.example"
(cd "$TMP/fifo-root" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -czf "$TMP/fifo.tar.gz" -C "$TMP/fifo-root" .
if bash "$ROOT/repka/verify_arm64_bundle.sh" "$TMP/fifo.tar.gz" >/dev/null 2>&1; then
 echo 'FAIL: FIFO archive member accepted' >&2; exit 1
fi

printf 'PASS: bundle snapshot, strict sidecar binding, pre-install path checks, archive entry types and base staging.\n'
