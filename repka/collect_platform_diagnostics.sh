#!/usr/bin/env bash
# Read-only Linux platform inventory for DockStation.
# Writes only the report directory, archive and checksum selected by the caller.
set -uo pipefail

ROOT_PREFIX="$(printenv DOCKSTATION_DIAG_ROOT 2>/dev/null || true)"
SKIP_PROBES="$(printenv DOCKSTATION_DIAG_SKIP_PROBES 2>/dev/null || printf '0')"
COLLECTOR_VERSION="1.0"
COMMAND_TIMEOUT_SECONDS="12"

root_path() {
    if [[ -n "$ROOT_PREFIX" ]]; then
        printf '%s%s\n' "$ROOT_PREFIX" "$1"
    else
        printf '%s\n' "$1"
    fi
}

read_property() {
    if [[ -r "$1" ]]; then
        tr '\000' ' ' < "$1" 2>/dev/null | tr -cd '[:print:]\t' | sed 's/[[:space:]]\+$//'
    else
        printf '[unavailable]'
    fi
}

list_sysfs_entries() {
    local title="$1" directory="$2" filter="$3"
    printf '\n[%s]\n' "$title"
    if [[ ! -d "$directory" ]]; then
        printf '[UNAVAILABLE] %s\n' "$directory"
        return
    fi
    local count=0 entry name
    while IFS= read -r entry; do
        name="$(basename -- "$entry")"
        if [[ -n "$filter" ]] && [[ ! "$name" =~ $filter ]]; then
            continue
        fi
        printf '%s\n' "$name"
        ((count += 1))
        if (( count >= 300 )); then
            printf '[TRUNCATED] listing limited to 300 entries\n'
            break
        fi
    done < <(find "$directory" -mindepth 1 -maxdepth 1 -print 2>/dev/null | LC_ALL=C sort)
    (( count > 0 )) || printf '[NONE] no entries found\n'
}

capture_command() {
    local output_file="$1"
    shift
    {
        printf '# command:'
        printf ' %q' "$@"
        printf '\n'
        if [[ "$SKIP_PROBES" == "1" ]]; then
            printf '[SKIPPED] external command probes disabled for fixture testing\n'
        elif ! command -v "$1" >/dev/null 2>&1; then
            printf '[UNAVAILABLE] command not found: %s\n' "$1"
        else
            local status=0
            if command -v timeout >/dev/null 2>&1; then
                timeout --signal=TERM "$COMMAND_TIMEOUT_SECONDS" "$@" || status=$?
            else
                "$@" || status=$?
            fi
            printf '\n# exit_status=%s\n' "$status"
        fi
    } > "$output_file" 2>&1
    chmod 600 "$output_file" 2>/dev/null || true
}

usage() {
    printf 'Usage: bash repka/collect_platform_diagnostics.sh [output-directory]\n'
    printf 'Creates a read-only Linux inventory directory, .tar.gz archive and SHA-256 sidecar.\n'
    printf 'Run without sudo. No packages, drivers, boot settings, permissions or services are changed.\n'
}

if [[ "$#" -gt 1 ]]; then
    usage >&2
    exit 2
fi
if [[ "$#" -eq 1 ]]; then
    case "$1" in
        -h|--help) usage; exit 0 ;;
    esac
fi

TIMESTAMP="$(date -u '+%Y%m%dT%H%M%SZ' 2>/dev/null || printf 'unknown-time')"
if [[ "$#" -eq 1 ]]; then
    OUTPUT_REQUESTED="$1"
else
    OUTPUT_REQUESTED="./dockstation-platform-diagnostics-$TIMESTAMP"
fi
ARCHIVE_REQUESTED="$OUTPUT_REQUESTED.tar.gz"
if [[ -e "$OUTPUT_REQUESTED" || -e "$ARCHIVE_REQUESTED" ]]; then
    printf 'ERROR: output path or archive already exists; refusing to overwrite: %s\n' "$OUTPUT_REQUESTED" >&2
    exit 2
fi
if ! mkdir -m 700 -p -- "$OUTPUT_REQUESTED"; then
    printf 'ERROR: cannot create output directory: %s\n' "$OUTPUT_REQUESTED" >&2
    exit 2
fi
OUTPUT_DIR="$(cd -- "$OUTPUT_REQUESTED" && pwd -P)"
ARCHIVE_PATH="$OUTPUT_DIR.tar.gz"
umask 077

{
    printf 'DockStation Linux platform diagnostics\n'
    printf 'Collector version: %s\n' "$COLLECTOR_VERSION"
    printf 'Collected UTC: %s\n' "$TIMESTAMP"
    if [[ -n "$ROOT_PREFIX" ]]; then
        printf 'Collection mode: filesystem-root override (test/offline inspection)\n'
    else
        printf 'Collection mode: live system\n'
    fi
    printf '\nThis report is observational only. It does not prove physical pin routing, electrical levels,\n'
    printf 'camera operation, hardware acceleration or successful communication with peripherals.\n'
    printf '\nPrivacy: full kernel command line, network addresses, hostnames, machine IDs, MAC addresses,\n'
    printf 'device serial numbers, credentials, keys, environment variables and service journals are not collected.\n'
    printf 'Only an explicit allowlist of non-secret kernel command-line options is included.\n'
} > "$OUTPUT_DIR/00_SUMMARY.txt"

OS_RELEASE="$(root_path /etc/os-release)"
{
    printf '[Selected /etc/os-release fields; the file is read as data, never executed]\n'
    if [[ -r "$OS_RELEASE" ]]; then
        sed -n -E '/^(PRETTY_NAME|NAME|ID|ID_LIKE|VERSION_ID|VERSION_CODENAME|UBUNTU_CODENAME)=/p' "$OS_RELEASE"
    else
        printf '[UNAVAILABLE] %s\n' "$OS_RELEASE"
    fi
    printf '\n[Kernel and process ABI]\n'
    if [[ "$SKIP_PROBES" == "1" ]]; then
        printf '[SKIPPED]\n'
    else
        uname -s -r -v -m -o 2>&1 || true
        printf 'LONG_BIT='
        getconf LONG_BIT 2>/dev/null || printf '[unavailable]\n'
        printf 'libc='
        getconf GNU_LIBC_VERSION 2>/dev/null || printf '[unavailable]\n'
    fi
    printf '\n[systemd]\n'
    if [[ "$SKIP_PROBES" == "1" ]]; then
        printf '[SKIPPED]\n'
    elif command -v systemctl >/dev/null 2>&1; then
        systemctl --version 2>&1 | head -n 2
    else
        printf '[UNAVAILABLE] systemctl\n'
    fi
    printf '\n[OpenSSL]\n'
    if [[ "$SKIP_PROBES" == "1" ]]; then
        printf '[SKIPPED]\n'
    elif command -v openssl >/dev/null 2>&1; then
        openssl version -a 2>&1 | head -n 4
    else
        printf '[UNAVAILABLE] openssl\n'
    fi
} > "$OUTPUT_DIR/01_SYSTEM.txt" 2>&1

CPUINFO="$(root_path /proc/cpuinfo)"
{
    printf '[Selected /proc/cpuinfo fields; serial and machine-id fields are excluded]\n'
    if [[ -r "$CPUINFO" ]]; then
        awk -F: '/^(processor|model name|Hardware|Revision|CPU implementer|CPU architecture|CPU variant|CPU part|Features)[[:space:]]*:/ { key=$1; value=$2; gsub(/^[[:space:]]+|[[:space:]]+$/, "", value); printf "%s: %s\n", key, value }' "$CPUINFO" | head -n 120
    else
        printf '[UNAVAILABLE] %s\n' "$CPUINFO"
    fi
    printf '\n[Memory]\n'
    if [[ -r "$(root_path /proc/meminfo)" ]]; then
        awk '/^(MemTotal|MemAvailable|SwapTotal|SwapFree):/ { print }' "$(root_path /proc/meminfo)"
    else
        printf '[UNAVAILABLE]\n'
    fi
} > "$OUTPUT_DIR/02_CPU_MEMORY.txt" 2>&1

{
    printf '[Kernel release and version]\n'
    if [[ "$SKIP_PROBES" == "1" ]]; then
        printf '[SKIPPED]\n'
    else
        uname -r 2>&1 || true
        uname -v 2>&1 || true
    fi
    printf '\n[Loaded modules from /proc/modules]\n'
    MODULES="$(root_path /proc/modules)"
    if [[ -r "$MODULES" ]]; then
        awk '{ printf "%s size=%s use_count=%s users=%s state=%s\n", $1, $2, $3, $4, $5 }' "$MODULES" | head -n 500
    else
        printf '[UNAVAILABLE] %s\n' "$MODULES"
    fi
    printf '\n[Relevant kernel configuration]\n'
    if [[ "$SKIP_PROBES" == "1" ]]; then
        printf '[SKIPPED]\n'
    else
        KERNEL_RELEASE="$(uname -r 2>/dev/null || true)"
        CONFIG_FILE="$(root_path "/boot/config-$KERNEL_RELEASE")"
        if [[ -r "$CONFIG_FILE" ]]; then
            grep -E '^(CONFIG_(OF|PINCTRL|GPIOLIB|GPIO|I2C|SPI|SERIAL|TTY|PWM|MEDIA|VIDEO|V4L2|ROCKCHIP|DRM|USB|PCIE|THERMAL|REGULATOR|MFD|MALI|PANFROST|SND|FW_LOADER)[A-Z0-9_]*=|# CONFIG_(OF|PINCTRL|GPIOLIB|GPIO|I2C|SPI|SERIAL|PWM|MEDIA|VIDEO|V4L2|ROCKCHIP|DRM|USB|PCIE|THERMAL|MALI|PANFROST)[A-Z0-9_]* is not set)' "$CONFIG_FILE" | head -n 400
        elif [[ -r "$(root_path /proc/config.gz)" ]] && command -v zcat >/dev/null 2>&1; then
            zcat "$(root_path /proc/config.gz)" 2>/dev/null | grep -E '^(CONFIG_(OF|PINCTRL|GPIOLIB|GPIO|I2C|SPI|SERIAL|PWM|MEDIA|VIDEO|V4L2|ROCKCHIP|DRM|USB|PCIE|THERMAL|MALI|PANFROST)[A-Z0-9_]*=|# CONFIG_(OF|PINCTRL|GPIOLIB|GPIO|I2C|SPI|SERIAL|PWM|MEDIA|VIDEO|V4L2|ROCKCHIP|DRM|USB|PCIE|THERMAL|MALI|PANFROST)[A-Z0-9_]* is not set)' | head -n 400
        else
            printf '[UNAVAILABLE] kernel config file not found/readable\n'
        fi
    fi
} > "$OUTPUT_DIR/03_KERNEL_MODULES.txt" 2>&1

DT_ROOT=""
for candidate in "$(root_path /sys/firmware/devicetree/base)" "$(root_path /proc/device-tree)"; do
    if [[ -d "$candidate" ]]; then
        DT_ROOT="$candidate"
        break
    fi
done
{
    printf '[Active Device Tree]\n'
    if [[ -n "$DT_ROOT" ]]; then
        printf 'path=%s\n' "$DT_ROOT"
        printf 'model=%s\n' "$(read_property "$DT_ROOT/model")"
        printf 'compatible=%s\n' "$(read_property "$DT_ROOT/compatible")"
        printf '\n[Aliases]\n'
        if [[ -d "$DT_ROOT/aliases" ]]; then
            while IFS= read -r alias_file; do
                [[ -f "$alias_file" ]] || continue
                printf '%s=%s\n' "$(basename -- "$alias_file")" "$(read_property "$alias_file")"
            done < <(find "$DT_ROOT/aliases" -mindepth 1 -maxdepth 1 -type f -print 2>/dev/null | LC_ALL=C sort)
        else
            printf '[UNAVAILABLE] aliases node\n'
        fi
        printf '\n[Relevant peripheral nodes: path, compatible, status]\n'
        node_count=0
        while IFS= read -r node; do
            relative="$(printf '%s' "$node" | sed "s|^$DT_ROOT||")"
            lower_relative="$(printf '%s' "$relative" | tr '[:upper:]' '[:lower:]')"
            case "$lower_relative" in
                *serial*|*uart*|*gpio*|*i2c*|*spi*|*pwm*|*camera*|*csi*|*dsi*|*video*|*vop*|*rga*|*mpp*|*codec*|*thermal*|*usb*|*pcie*|*ethernet*|*gmac*|*pinctrl*) ;;
                *) continue ;;
            esac
            printf '%s | name=%s | compatible=%s | status=%s\n' "$relative" \
                "$(read_property "$node/name")" \
                "$(read_property "$node/compatible")" \
                "$(read_property "$node/status")"
            ((node_count += 1))
            if (( node_count >= 250 )); then
                printf '[TRUNCATED] relevant Device Tree nodes limited to 250\n'
                break
            fi
        done < <(find "$DT_ROOT" -type d -print 2>/dev/null | LC_ALL=C sort)
        (( node_count > 0 )) || printf '[NONE] no matching nodes found\n'
    else
        printf '[UNAVAILABLE] neither active Device Tree path is readable\n'
    fi
    printf '\n[Device Tree overlays]\n'
    OVERLAY_ROOT="$(root_path /sys/kernel/config/device-tree/overlays)"
    if [[ -d "$OVERLAY_ROOT" ]]; then
        find "$OVERLAY_ROOT" -mindepth 1 -maxdepth 1 -printf '%f\n' 2>/dev/null | LC_ALL=C sort
    else
        printf '[UNAVAILABLE] configfs overlay directory absent; boot-time overlays may still be active\n'
    fi
} > "$OUTPUT_DIR/04_DEVICE_TREE.txt" 2>&1

{
    printf '[Kernel command line: allowlisted options only]\n'
    CMDLINE="$(root_path /proc/cmdline)"
    if [[ -r "$CMDLINE" ]]; then
        tr ' ' '\n' < "$CMDLINE" | grep -E '^(console|earlycon|rootfstype|rootwait|loglevel|iommu|iommu\.[A-Za-z0-9_.-]+|cgroup\.[A-Za-z0-9_.-]+|cgroup_[A-Za-z0-9_]+|systemd\.unified_cgroup_hierarchy|systemd\.legacy_systemd_cgroup_controller|systemd\.log_level|systemd\.log_target|systemd\.show_status|clk_ignore_unused|pcie_aspm|video|drm\.[A-Za-z0-9_.-]+|usbcore\.[A-Za-z0-9_.-]+|coherent_pool)=|^(ro|rw|quiet|splash)$' || true
    else
        printf '[UNAVAILABLE] %s\n' "$CMDLINE"
    fi
    printf '\n[Boot file inventory; contents are not copied]\n'
    for boot_dir in "$(root_path /boot)" "$(root_path /boot/firmware)" "$(root_path /boot/extlinux)"; do
        [[ -d "$boot_dir" ]] || continue
        printf '%s\n' "$boot_dir"
        find "$boot_dir" -maxdepth 3 -type f \( -name '*.dtb' -o -name '*.dtbo' -o -name 'extlinux.conf' -o -name 'boot.cmd' -o -name 'uEnv.txt' -o -name 'armbianEnv.txt' -o -name 'config.txt' -o -name 'cmdline.txt' -o -name 'Image' -o -name 'vmlinuz*' -o -name 'initrd*' \) -printf '  %P (%s bytes)\n' 2>/dev/null | head -n 250
    done
} > "$OUTPUT_DIR/05_BOOT.txt" 2>&1

{
    printf '[Device nodes: presence does not prove physical pin routing]\n'
    shopt -s nullglob
    found=0
    for item in "$(root_path /dev)"/serial0 "$(root_path /dev)"/serial/by-id/* "$(root_path /dev)"/serial/by-path/* \
        "$(root_path /dev)"/ttyS* "$(root_path /dev)"/ttyAMA* "$(root_path /dev)"/ttyFIQ* \
        "$(root_path /dev)"/ttyUSB* "$(root_path /dev)"/ttyACM* "$(root_path /dev)"/gpiochip* \
        "$(root_path /dev)"/i2c-* "$(root_path /dev)"/spidev* "$(root_path /dev)"/pwm* \
        "$(root_path /dev)"/video* "$(root_path /dev)"/media*; do
        [[ -e "$item" || -L "$item" ]] || continue
        resolved="$(readlink -f -- "$item" 2>/dev/null || printf '%s' "$item")"
        metadata="$(stat -Lc 'type=%F mode=%a uid=%u gid=%g rdev=%t:%T' -- "$item" 2>/dev/null || printf 'metadata unavailable')"
        printf '%s -> %s (%s)\n' "$item" "$resolved" "$metadata"
        ((found += 1))
    done
    shopt -u nullglob
    (( found > 0 )) || printf '[NONE] no matching device nodes found\n'
    printf '\n[TTY and UART sysfs]\n'
    TTY_ROOT="$(root_path /sys/class/tty)"
    if [[ -d "$TTY_ROOT" ]]; then
        while IFS= read -r tty_entry; do
            tty_name="$(basename -- "$tty_entry")"
            [[ "$tty_name" =~ ^(ttyS|ttyAMA|ttyFIQ|ttyUSB|ttyACM|serial) ]] || continue
            driver='[unavailable]'
            if [[ -L "$tty_entry/device/driver" ]]; then
                driver="$(readlink -f "$tty_entry/device/driver" 2>/dev/null || true)"
            fi
            of_node='[unavailable]'
            if [[ -L "$tty_entry/device/of_node" ]]; then
                of_node="$(readlink -f "$tty_entry/device/of_node" 2>/dev/null || true)"
            fi
            printf '%s driver=%s of_node=%s\n' "$tty_name" "$driver" "$of_node"
        done < <(find "$TTY_ROOT" -mindepth 1 -maxdepth 1 -type d -print 2>/dev/null | LC_ALL=C sort)
    else
        printf '[UNAVAILABLE] %s\n' "$TTY_ROOT"
    fi
    list_sysfs_entries 'GPIO chips' "$(root_path /sys/class/gpio)" ''
    list_sysfs_entries 'I2C adapters' "$(root_path /sys/class/i2c-adapter)" ''
    list_sysfs_entries 'I2C devices' "$(root_path /sys/bus/i2c/devices)" ''
    list_sysfs_entries 'SPI devices' "$(root_path /sys/bus/spi/devices)" ''
    list_sysfs_entries 'PWM controllers' "$(root_path /sys/class/pwm)" ''
    list_sysfs_entries 'Video4Linux devices' "$(root_path /sys/class/video4linux)" ''
    list_sysfs_entries 'Media devices' "$(root_path /sys/class/media)" ''
    list_sysfs_entries 'Relevant platform devices' "$(root_path /sys/bus/platform/devices)" '(uart|serial|gpio|i2c|spi|pwm|csi|camera|video|vop|rga|mpp|codec|thermal|usb|pcie|gmac|ethernet|pinctrl)'
    printf '\n[Thermal zones]\n'
    THERMAL_ROOT="$(root_path /sys/class/thermal)"
    if [[ -d "$THERMAL_ROOT" ]]; then
        while IFS= read -r zone; do
            [[ -d "$zone" ]] || continue
            printf '%s type=%s temp_millidegrees=%s\n' "$(basename -- "$zone")" \
                "$(read_property "$zone/type")" "$(read_property "$zone/temp")"
        done < <(find "$THERMAL_ROOT" -mindepth 1 -maxdepth 1 -type d -name 'thermal_zone*' -print 2>/dev/null | LC_ALL=C sort)
    else
        printf '[UNAVAILABLE] %s\n' "$THERMAL_ROOT"
    fi
} > "$OUTPUT_DIR/06_INTERFACES.txt" 2>&1

capture_command "$OUTPUT_DIR/07_PACKAGES.txt" dpkg-query -W
if [[ -f "$OUTPUT_DIR/07_PACKAGES.txt" && "$SKIP_PROBES" != "1" ]] && command -v dpkg-query >/dev/null 2>&1; then
    grep -Ei '^(linux-(image|modules|headers)|libc6 |systemd |libsystemd|openssl |libssl|libcamera|gstreamer|libgstreamer|rockchip|lib(rga|mpp)|mpp |wiringrp|wiringpi|libgpiod|gpiod |v4l-utils|libv4l|device-tree-compiler|ffmpeg|cmake|gcc-[0-9]|g\+\+-[0-9])' \
        "$OUTPUT_DIR/07_PACKAGES.txt" > "$OUTPUT_DIR/07_PACKAGES.filtered.txt" || true
    if [[ -s "$OUTPUT_DIR/07_PACKAGES.filtered.txt" ]]; then
        mv "$OUTPUT_DIR/07_PACKAGES.filtered.txt" "$OUTPUT_DIR/07_PACKAGES.txt"
    else
        printf '\n[No matching platform packages reported by dpkg-query]\n' >> "$OUTPUT_DIR/07_PACKAGES.txt"
        rm -f "$OUTPUT_DIR/07_PACKAGES.filtered.txt"
    fi
fi
capture_command "$OUTPUT_DIR/08_SHARED_LIBRARIES.txt" bash -c 'if command -v ldconfig >/dev/null 2>&1; then ldconfig -p | grep -Ei "(rockchip|mpp|rga|camera|gstreamer|gst|v4l|gpiod|wiring|drm|vulkan|mali|panfrost)" || true; else echo "[UNAVAILABLE] ldconfig"; fi'

{
    printf '[Available diagnostic tools]\n'
    for tool in dtc repka-config gpioinfo gpiodetect i2cdetect v4l2-ctl media-ctl gst-inspect-1.0 ffmpeg ffprobe lsusb lspci modinfo; do
        if [[ "$SKIP_PROBES" == "1" ]]; then
            printf '%s: [SKIPPED]\n' "$tool"
        elif command -v "$tool" >/dev/null 2>&1; then
            printf '%s: %s\n' "$tool" "$(command -v "$tool")"
        else
            printf '%s: [not installed]\n' "$tool"
        fi
    done
} > "$OUTPUT_DIR/09_TOOLS.txt"

capture_command "$OUTPUT_DIR/10_GPIO_I2C.txt" bash -c '
set +e
if command -v gpiodetect >/dev/null 2>&1; then echo "[gpiodetect]"; gpiodetect; else echo "[gpiodetect unavailable]"; fi
if command -v gpioinfo >/dev/null 2>&1; then echo "[gpioinfo: metadata only; no line values are requested]"; gpioinfo; else echo "[gpioinfo unavailable]"; fi
if command -v i2cdetect >/dev/null 2>&1; then echo "[i2cdetect -l: adapter list only; no bus scan]"; i2cdetect -l; else echo "[i2cdetect unavailable]"; fi
'
capture_command "$OUTPUT_DIR/11_MULTIMEDIA.txt" bash -c '
set +e
if command -v v4l2-ctl >/dev/null 2>&1; then echo "[V4L2 devices]"; v4l2-ctl --list-devices; else echo "[v4l2-ctl unavailable]"; fi
if command -v media-ctl >/dev/null 2>&1; then echo "[Media topology]"; media-ctl --print-topology; else echo "[media-ctl unavailable]"; fi
if command -v gst-inspect-1.0 >/dev/null 2>&1; then echo "[Relevant GStreamer plugins/elements]"; gst-inspect-1.0 2>/dev/null | grep -Ei "(rockchip|mpp|rga|v4l2|camera|libcamera|vaapi|vulkan|openh264|x264|x265|jpeg|drm)" | head -n 200; else echo "[gst-inspect-1.0 unavailable]"; fi
if command -v ffmpeg >/dev/null 2>&1; then echo "[FFmpeg hardware acceleration declarations]"; ffmpeg -hide_banner -hwaccels 2>&1; else echo "[ffmpeg unavailable]"; fi
if command -v lsusb >/dev/null 2>&1; then echo "[USB inventory]"; lsusb; else echo "[lsusb unavailable]"; fi
if command -v lspci >/dev/null 2>&1; then echo "[PCI inventory]"; lspci -nnk; else echo "[lspci unavailable]"; fi
'
capture_command "$OUTPUT_DIR/12_RELEVANT_SERVICES.txt" bash -c '
set +e
if command -v systemctl >/dev/null 2>&1; then
    systemctl --no-pager --plain list-units --type=service --all 2>&1 | grep -Ei "(repka|rockchip|camera|media|video|gstreamer|udev|ssh|dockstation|networkmanager)" || true
else
    echo "[UNAVAILABLE] systemctl"
fi
'

{
    printf '[Known interpretation limits]\n'
    printf '* Device nodes or loaded modules do not prove physical pin routing to a connector.\n'
    printf '* Device Tree status describes the kernel hardware description, not electrical correctness.\n'
    printf '* GStreamer/FFmpeg plugin presence does not prove the hardware codec path works.\n'
    printf '* USB/PCI enumeration and V4L2 metadata do not prove an end-to-end camera stream.\n'
    printf '* gpioinfo and i2cdetect run only in read-only listing modes; no GPIO values are changed and no I2C bus scan is performed.\n'
    printf '* Missing permissions are reported as unavailable; running as root is not required.\n'
} > "$OUTPUT_DIR/13_LIMITATIONS.txt"

find "$OUTPUT_DIR" -type f -exec chmod 600 {} + 2>/dev/null || true
if ! command -v tar >/dev/null 2>&1; then
    printf 'ERROR: tar is unavailable; report directory remains at %s\n' "$OUTPUT_DIR" >&2
    exit 1
fi
if ! tar -czf "$ARCHIVE_PATH" -C "$(dirname -- "$OUTPUT_DIR")" "$(basename -- "$OUTPUT_DIR")"; then
    printf 'ERROR: cannot create report archive; report directory remains at %s\n' "$OUTPUT_DIR" >&2
    exit 1
fi
chmod 600 "$ARCHIVE_PATH" 2>/dev/null || true
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$ARCHIVE_PATH" > "$ARCHIVE_PATH.sha256"
    chmod 600 "$ARCHIVE_PATH.sha256" 2>/dev/null || true
fi
printf 'DockStation platform diagnostics completed.\n'
printf 'Report directory: %s\nArchive: %s\n' "$OUTPUT_DIR" "$ARCHIVE_PATH"
if [[ -f "$ARCHIVE_PATH.sha256" ]]; then printf 'Checksum: %s\n' "$ARCHIVE_PATH.sha256"; fi
printf 'No system configuration, packages, drivers, permissions or services were changed.\n'
exit 0
