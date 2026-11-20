#!/usr/bin/env bash
set -euo pipefail

ROOT=""
OUTPUT_DIR=""
VERSION="${DOCKSTATION_DEB_VERSION:-}"
while (($#)); do
    case "$1" in
        --root) ROOT="${2:?missing value for --root}"; shift 2 ;;
        --output-dir) OUTPUT_DIR="${2:?missing value for --output-dir}"; shift 2 ;;
        --version) VERSION="${2:?missing value for --version}"; shift 2 ;;
        --help)
            echo "Usage: $0 --root STAGED_INSTALL_ROOT --output-dir DIR [--version DEBIAN_VERSION]"
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$ROOT" && -d "$ROOT" ]] || { echo "A staged install root is required." >&2; exit 2; }
[[ -n "$OUTPUT_DIR" ]] || { echo "An output directory is required." >&2; exit 2; }
[[ -n "$VERSION" ]] || VERSION="0.1.0~git$(git rev-parse --short=12 HEAD)"
[[ "$VERSION" =~ ^[0-9][A-Za-z0-9.+:~_-]*$ ]] || { echo "Invalid Debian package version: $VERSION" >&2; exit 2; }

for required in \
    usr/bin/dockstation_linux_bootstrap \
    usr/bin/dockstation-credential-provision \
    usr/bin/dockstation-registry-provision \
    usr/lib/systemd/system/dockstation-gateway.service \
    usr/lib/systemd/system/dockstation-gateway-uart@.service \
    usr/share/dockstation/gateway.conf.example; do
    [[ -f "$ROOT/$required" && ! -L "$ROOT/$required" ]] ||
        { echo "Missing, non-regular, or symlinked staged package payload: $required" >&2; exit 1; }
done
for binary in dockstation_linux_bootstrap dockstation-credential-provision dockstation-registry-provision; do
    [[ -x "$ROOT/usr/bin/$binary" ]] ||
        { echo "Staged executable is not executable: $binary" >&2; exit 1; }
done

command -v dpkg-deb >/dev/null
command -v dpkg-shlibdeps >/dev/null
command -v install >/dev/null
command -v readelf >/dev/null

PACKAGE_ROOT="$(mktemp -d)"
trap 'rm -rf -- "$PACKAGE_ROOT"' EXIT
mkdir -p "$OUTPUT_DIR" "$PACKAGE_ROOT/DEBIAN" "$PACKAGE_ROOT/etc/dockstation"

# Copy an explicit allow-list only. Do not package extra staged files,
# build trees, tests, firmware binaries, keys, local credentials, or host config.
mkdir -p "$PACKAGE_ROOT/usr/bin" "$PACKAGE_ROOT/usr/lib/systemd/system" \
    "$PACKAGE_ROOT/usr/share/dockstation"
for binary in dockstation_linux_bootstrap dockstation-credential-provision dockstation-registry-provision; do
    install -m 0755 "$ROOT/usr/bin/$binary" "$PACKAGE_ROOT/usr/bin/$binary"
done
install -m 0644 "$ROOT/usr/lib/systemd/system/dockstation-gateway.service" \
    "$PACKAGE_ROOT/usr/lib/systemd/system/dockstation-gateway.service"
install -m 0644 "$ROOT/usr/lib/systemd/system/dockstation-gateway-uart@.service" \
    "$PACKAGE_ROOT/usr/lib/systemd/system/dockstation-gateway-uart@.service"
install -m 0644 "$ROOT/usr/share/dockstation/gateway.conf.example" \
    "$PACKAGE_ROOT/usr/share/dockstation/gateway.conf.example"
install -m 0644 "$ROOT/usr/share/dockstation/gateway.conf.example" \
    "$PACKAGE_ROOT/etc/dockstation/gateway.conf"
install -m 0644 packaging/debian/conffiles "$PACKAGE_ROOT/DEBIAN/conffiles"
for script in postinst prerm postrm; do
    install -m 0755 "packaging/debian/$script" "$PACKAGE_ROOT/DEBIAN/$script"
done

# dpkg-shlibdeps reads Debian metadata from a temporary work tree so this
# helper never creates or overwrites a repository-root debian/ directory.
DEBIAN_METADATA_ROOT="$(mktemp -d)"
mkdir -p "$DEBIAN_METADATA_ROOT/debian"
cp packaging/debian/shlibdeps.control "$DEBIAN_METADATA_ROOT/debian/control"
printf 'dockstation-gateway (%s) unstable; urgency=medium\n\n  * CI package build.\n\n -- DockStation Project <noreply@localhost>  Thu, 19 Nov 2026 00:00:00 +0000\n' "$VERSION" > "$DEBIAN_METADATA_ROOT/debian/changelog"
trap 'rm -rf -- "$PACKAGE_ROOT" "$DEBIAN_METADATA_ROOT"' EXIT

SHLIBS_OUTPUT="$(cd "$DEBIAN_METADATA_ROOT" && dpkg-shlibdeps -O \
    -e"$PACKAGE_ROOT/usr/bin/dockstation_linux_bootstrap" \
    -e"$PACKAGE_ROOT/usr/bin/dockstation-credential-provision" \
    -e"$PACKAGE_ROOT/usr/bin/dockstation-registry-provision")"
RUNTIME_DEPENDS="$(printf '%s\n' "$SHLIBS_OUTPUT" | sed -n 's/^shlibs:Depends=//p')"
[[ -n "$RUNTIME_DEPENDS" ]] || { echo "dpkg-shlibdeps returned no runtime dependencies." >&2; exit 1; }
# Debian 12 uses libssl3; Ubuntu 24.04 uses the t64-renamed libssl3t64.
# Keep the package installable on either ABI-compatible OpenSSL 3 provider.
RUNTIME_DEPENDS="$(printf '%s\n' "$RUNTIME_DEPENDS" | sed -E 's/libssl3t64 \(>= ([^)]+)\)/libssl3 (>= \1) | libssl3t64 (>= \1)/g')"
# Build a native package only: Debian architecture metadata must match every
# shipped ELF executable. Cross-compilation/package staging is not implied.
PACKAGE_ARCH="$(dpkg --print-architecture)"
case "$PACKAGE_ARCH" in
    amd64)
        EXPECTED_ELF_MACHINE='Advanced Micro Devices X86-64'
        ;;
    arm64)
        EXPECTED_ELF_MACHINE='AArch64'
        ;;
    *)
        echo "Unsupported Debian package architecture: $PACKAGE_ARCH (supported: amd64, arm64)" >&2
        exit 1
        ;;
esac

sed -e "s/@VERSION@/$VERSION/g" \
    -e "s/@ARCH@/$PACKAGE_ARCH/g" \
    -e "s#@RUNTIME_DEPENDS@#$RUNTIME_DEPENDS#g" \
    packaging/debian/control.in > "$PACKAGE_ROOT/DEBIAN/control"

for binary in "$PACKAGE_ROOT"/usr/bin/dockstation*; do
    readelf -h "$binary" | grep -Eq "Machine:[[:space:]]+$EXPECTED_ELF_MACHINE$" || {
        echo "ELF architecture mismatch: $binary is not $PACKAGE_ARCH." >&2
        readelf -h "$binary" | grep 'Machine:' >&2 || true
        exit 1
    }
done

chmod 0755 "$PACKAGE_ROOT/DEBIAN/postinst" "$PACKAGE_ROOT/DEBIAN/prerm" "$PACKAGE_ROOT/DEBIAN/postrm"
chmod 0644 "$PACKAGE_ROOT/DEBIAN/control" "$PACKAGE_ROOT/DEBIAN/conffiles"
package="$OUTPUT_DIR/dockstation-gateway_${VERSION}_${PACKAGE_ARCH}.deb"
dpkg-deb --root-owner-group --build "$PACKAGE_ROOT" "$package"
dpkg-deb --info "$package"
dpkg-deb --contents "$package"
(
    cd "$OUTPUT_DIR"
    package_name="$(basename "$package")"
    sha256sum "$package_name" > "$package_name.sha256"
    sha256sum --check --strict "$package_name.sha256"
)
echo "Created $package"
