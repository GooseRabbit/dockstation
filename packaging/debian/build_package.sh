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
    [[ -f "$ROOT/$required" ]] || { echo "Missing staged package payload: $required" >&2; exit 1; }
done

command -v dpkg-deb >/dev/null
command -v dpkg-shlibdeps >/dev/null
command -v install >/dev/null
command -v readelf >/dev/null

PACKAGE_ROOT="$(mktemp -d)"
trap 'rm -rf -- "$PACKAGE_ROOT"' EXIT
mkdir -p "$OUTPUT_DIR" "$PACKAGE_ROOT/DEBIAN" "$PACKAGE_ROOT/etc/dockstation"

# Copy only the reviewed production payload. Do not package build trees, tests,
# firmware binaries, keys, local credentials, or host-specific config.
cp -a "$ROOT/usr" "$PACKAGE_ROOT/"
install -m 0644 "$ROOT/usr/share/dockstation/gateway.conf.example" \
    "$PACKAGE_ROOT/etc/dockstation/gateway.conf"
install -m 0644 packaging/debian/conffiles "$PACKAGE_ROOT/DEBIAN/conffiles"
for script in postinst prerm postrm; do
    install -m 0755 "packaging/debian/$script" "$PACKAGE_ROOT/DEBIAN/$script"
done

# dpkg-shlibdeps derives runtime package dependencies from the actual ARM64 ELF binaries.
mkdir -p debian
cp packaging/debian/shlibdeps.control debian/control
cleanup_debian_control() { rm -f debian/control debian/substvars; }
trap 'cleanup_debian_control; rm -rf -- "$PACKAGE_ROOT"' EXIT

SHLIBS_OUTPUT="$(dpkg-shlibdeps -O \
    -e"$PACKAGE_ROOT/usr/bin/dockstation_linux_bootstrap" \
    -e"$PACKAGE_ROOT/usr/bin/dockstation-credential-provision" \
    -e"$PACKAGE_ROOT/usr/bin/dockstation-registry-provision")"
RUNTIME_DEPENDS="$(printf '%s\n' "$SHLIBS_OUTPUT" | sed -n 's/^shlibs:Depends=//p')"
[[ -n "$RUNTIME_DEPENDS" ]] || { echo "dpkg-shlibdeps returned no runtime dependencies." >&2; exit 1; }
rm -f debian/control debian/substvars

sed -e "s/@VERSION@/$VERSION/g" \
    -e "s|@RUNTIME_DEPENDS@|$RUNTIME_DEPENDS|g" \
    packaging/debian/control.in > "$PACKAGE_ROOT/DEBIAN/control"

# Validate ELF architecture before creating a package labelled arm64.
for binary in "$PACKAGE_ROOT"/usr/bin/dockstation*; do
    readelf -h "$binary" | grep -Eq 'Machine:[[:space:]]+AArch64' || {
        echo "Non-AArch64 executable in ARM64 package: $binary" >&2
        exit 1
    }
done

chmod 0755 "$PACKAGE_ROOT/DEBIAN/postinst" "$PACKAGE_ROOT/DEBIAN/prerm" "$PACKAGE_ROOT/DEBIAN/postrm"
chmod 0644 "$PACKAGE_ROOT/DEBIAN/control" "$PACKAGE_ROOT/DEBIAN/conffiles"
package="$OUTPUT_DIR/dockstation-gateway_${VERSION}_arm64.deb"
dpkg-deb --root-owner-group --build "$PACKAGE_ROOT" "$package"
dpkg-deb --info "$package"
dpkg-deb --contents "$package"
sha256sum "$package" > "$package.sha256"
sha256sum --check --strict "$package.sha256"
echo "Created $package"
