#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_PREFLIGHT="${DOCKSTATION_PREFLIGHT_SCRIPT:-$REPO_ROOT/repka/check_target_platform.sh}"
RUNTIME_PREFLIGHT="${DOCKSTATION_RUNTIME_PREFLIGHT_SCRIPT:-$REPO_ROOT/repka/check_target_runtime.sh}"
BUNDLE_VERIFIER="${DOCKSTATION_BUNDLE_VERIFIER:-$REPO_ROOT/repka/verify_arm64_bundle.sh}"
ACCOUNT_PREFLIGHT="${DOCKSTATION_ACCOUNT_PREFLIGHT_SCRIPT:-$REPO_ROOT/repka/check_service_identity.sh}"
CMAKE="${DOCKSTATION_CMAKE_COMMAND:-cmake}"
SYSTEMCTL="${DOCKSTATION_SYSTEMCTL_COMMAND:-systemctl}"
SYSTEMD_ANALYZE="${DOCKSTATION_SYSTEMD_ANALYZE_COMMAND:-systemd-analyze}"
GETENT="${DOCKSTATION_GETENT_COMMAND:-getent}"
GROUPADD="${DOCKSTATION_GROUPADD_COMMAND:-groupadd}"
USERADD="${DOCKSTATION_USERADD_COMMAND:-useradd}"
INSTALL="${DOCKSTATION_INSTALL_COMMAND:-install}"
CHOWN="${DOCKSTATION_CHOWN_COMMAND:-chown}"
CHMOD="${DOCKSTATION_CHMOD_COMMAND:-chmod}"
BUILD_DIR="${DOCKSTATION_BUILD_DIR:-$REPO_ROOT/build}"
STAGE_ROOT="${DOCKSTATION_INSTALL_ROOT:-}"
BUILD_JOBS="${DOCKSTATION_BUILD_JOBS:-2}"
BUNDLE=""
BUNDLE_TEMP=""
BUNDLE_SNAPSHOT=""
PREFIX="/usr/local"
CONFIG_DIR="/etc/dockstation"
CONFIG_FILE="$CONFIG_DIR/gateway.conf"
BASE_UNIT="$PREFIX/lib/systemd/system/dockstation-gateway.service"
UART_UNIT="$PREFIX/lib/systemd/system/dockstation-gateway-uart@.service"
EXAMPLE_CONFIG="$PREFIX/share/dockstation/gateway.conf.example"

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*" >&2; }
cleanup() {
    if [[ -n "$BUNDLE_TEMP" && -d "$BUNDLE_TEMP" ]]; then
        rm -rf -- "$BUNDLE_TEMP"
    fi
}

if [[ "$#" -eq 2 && "$1" == "--bundle" ]]; then
    [[ -n "$2" ]] || fail "После --bundle требуется путь к архиву."
    BUNDLE="$2"
elif [[ "$#" -eq 1 && "$1" == "--help" ]]; then
    printf 'Usage: sudo bash repka/install_gateway.sh [--bundle /path/to/dockstation-repka-pi5-arm64.tar.gz]\n'
    exit 0
elif [[ "$#" -ne 0 ]]; then
    printf 'Usage: sudo bash repka/install_gateway.sh [--bundle /path/to/dockstation-repka-pi5-arm64.tar.gz]\n' >&2
    exit 2
fi

[[ -n "$STAGE_ROOT" || "$EUID" -eq 0 ]] ||
    fail "Запустите установку от root: sudo bash repka/install_gateway.sh"
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] ||
    fail "DOCKSTATION_BUILD_JOBS должен быть положительным целым числом."
[[ -z "$STAGE_ROOT" || "$STAGE_ROOT" == /* ]] ||
    fail "DOCKSTATION_INSTALL_ROOT должен быть абсолютным путём."

if [[ -n "$BUNDLE" ]]; then
    [[ -f "$BUNDLE" && ! -L "$BUNDLE" ]] ||
        fail "ARM64 bundle отсутствует, не является обычным файлом или является symlink: $BUNDLE"
    [[ -f "$BUNDLE.sha256" && ! -L "$BUNDLE.sha256" ]] ||
        fail "Sidecar SHA-256 отсутствует, не является обычным файлом или является symlink: $BUNDLE.sha256"
    [[ -x "$BUNDLE_VERIFIER" ]] || fail "Не найден исполняемый verifier bundle: $BUNDLE_VERIFIER"
    for tool in tar sha256sum mktemp cat chmod awk basename rm; do
        command -v "$tool" >/dev/null 2>&1 || fail "Не найдена команда $tool."
    done

    # Snapshot once into a private directory. Verification and extraction below
    # consume this exact file, never the caller-controlled source path again.
    BUNDLE_TEMP="$(mktemp -d)"
    trap cleanup EXIT
    BUNDLE_SNAPSHOT="$BUNDLE_TEMP/dockstation-repka-pi5-arm64.tar.gz"
    cat -- "$BUNDLE" > "$BUNDLE_SNAPSHOT"
    chmod 0400 "$BUNDLE_SNAPSHOT"

    mapfile -t checksum_lines < "$BUNDLE.sha256"
    [[ "${#checksum_lines[@]}" -eq 1 ]] ||
        fail "Sidecar должен содержать ровно одну строку SHA-256."
    checksum_line="${checksum_lines[0]}"
    [[ "${checksum_line:0:64}" =~ ^[[:xdigit:]]{64}$ && "${checksum_line:64:2}" == "  " ]] ||
        fail "Sidecar должен иметь формат sha256sum: 64 hex-символа, два пробела, имя файла."
    expected_hash="${checksum_line:0:64}"
    sidecar_name="${checksum_line:66}"
    bundle_name="$(basename -- "$BUNDLE")"
    [[ "$sidecar_name" == "$bundle_name" ]] ||
        fail "Sidecar относится к '$sidecar_name', а устанавливается '$bundle_name'."
    actual_hash="$(sha256sum < "$BUNDLE_SNAPSHOT" | awk '{ print $1 }')"
    [[ "${actual_hash,,}" == "${expected_hash,,}" ]] ||
        fail "Внешняя SHA-256 bundle не совпадает."

    "$BUNDLE_VERIFIER" "$BUNDLE_SNAPSHOT"
    PREFLIGHT="$RUNTIME_PREFLIGHT"
else
    PREFLIGHT="$SOURCE_PREFLIGHT"
fi

[[ -f "$PREFLIGHT" ]] || fail "Не найден preflight: $PREFLIGHT"
command -v "$INSTALL" >/dev/null 2>&1 || fail "Не найдена команда install."
command -v "$SYSTEMD_ANALYZE" >/dev/null 2>&1 || fail "Не найдена команда systemd-analyze."

# Validate destination paths before preflight/build/install can write anything.
TARGET_CONFIG_DIR="$STAGE_ROOT$CONFIG_DIR"
TARGET_CONFIG_FILE="$STAGE_ROOT$CONFIG_FILE"
if [[ -n "$STAGE_ROOT" ]]; then
    [[ ! -L "$STAGE_ROOT" ]] || fail "Staging root является symlink: $STAGE_ROOT"
fi
TARGET_DIRS=(
    "$STAGE_ROOT/usr"
    "$STAGE_ROOT/etc"
    "$STAGE_ROOT$PREFIX"
    "$STAGE_ROOT$PREFIX/bin"
    "$STAGE_ROOT$PREFIX/lib"
    "$STAGE_ROOT$PREFIX/lib/systemd"
    "$STAGE_ROOT$PREFIX/lib/systemd/system"
    "$STAGE_ROOT$PREFIX/share"
    "$STAGE_ROOT$PREFIX/share/dockstation"
    "$TARGET_CONFIG_DIR"
)
for target_dir in "${TARGET_DIRS[@]}"; do
    [[ ! -L "$target_dir" ]] || fail "$target_dir является symlink; установка запрещена."
    [[ ! -e "$target_dir" || -d "$target_dir" ]] ||
        fail "$target_dir существует, но не является каталогом."
done
TARGET_FILES=(
    "$STAGE_ROOT$PREFIX/bin/dockstation_linux_bootstrap"
    "$STAGE_ROOT$PREFIX/bin/dockstation-credential-provision"
    "$STAGE_ROOT$BASE_UNIT"
    "$STAGE_ROOT$UART_UNIT"
    "$STAGE_ROOT$EXAMPLE_CONFIG"
    "$TARGET_CONFIG_FILE"
)
for target_file in "${TARGET_FILES[@]}"; do
    [[ ! -L "$target_file" ]] || fail "$target_file является symlink; перезапись запрещена."
    [[ ! -e "$target_file" || -f "$target_file" ]] ||
        fail "$target_file существует, но не является обычным файлом."
done

# Exit 1 is warning-only (e.g. no verified UART); exit 2 is a hard blocker.
preflight_status=0
if "$PREFLIGHT"; then
    preflight_status=0
else
    preflight_status=$?
fi
((preflight_status <= 1)) ||
    fail "Preflight выявил обязательную проблему (код $preflight_status)."
if [[ "$preflight_status" -eq 1 ]]; then
    warn "Preflight сообщил предупреждения; аппаратный UART автоматически не включается."
fi

if [[ -n "$BUNDLE" ]]; then
    [[ -f "$BUNDLE" ]] || fail "ARM64 bundle не найден: $BUNDLE"
    [[ -f "$BUNDLE.sha256" ]] || fail "Не найден sidecar SHA-256: $BUNDLE.sha256"
    [[ -x "$BUNDLE_VERIFIER" ]] || fail "Не найден исполняемый verifier: $BUNDLE_VERIFIER"
    for tool in tar sha256sum mktemp; do
        command -v "$tool" >/dev/null 2>&1 || fail "Не найдена команда $tool."
    done
    (
        cd "$(dirname -- "$BUNDLE")"
        sha256sum --check --strict "$(basename -- "$BUNDLE").sha256"
    ) || fail "Внешняя SHA-256 bundle не совпадает."
    "$BUNDLE_VERIFIER" "$BUNDLE"
else
    command -v "$CMAKE" >/dev/null 2>&1 || fail "Не найдена команда CMake."
fi

ACTIVE_UART=""
BASE_ACTIVE=false
CONFIG_UART=false

if [[ -z "$STAGE_ROOT" ]]; then
    for cmd in "$SYSTEMCTL" "$GETENT" "$GROUPADD" "$USERADD" "$CHOWN" "$CHMOD"; do
        command -v "$cmd" >/dev/null 2>&1 || fail "Не найдена команда $cmd."
    done
    [[ -f "$ACCOUNT_PREFLIGHT" ]] || fail "Не найден account preflight: $ACCOUNT_PREFLIGHT"
    DOCKSTATION_GETENT_COMMAND="$GETENT" bash "$ACCOUNT_PREFLIGHT"

    if [[ -f "$CONFIG_FILE" ]] &&
       grep -Eq '^[[:space:]]*controller_uart_device[[:space:]]*=' "$CONFIG_FILE"; then
        CONFIG_UART=true
    fi

    active_list="$("$SYSTEMCTL" list-units --type=service --state=active \
        --no-legend --plain 'dockstation-gateway-uart@*.service' 2>/dev/null || true)"
    active_names=()
    if [[ -n "$active_list" ]]; then
        while IFS= read -r unit_name; do
            [[ -n "$unit_name" ]] && active_names+=("$unit_name")
        done < <(printf '%s\n' "$active_list" | awk 'NF { print $1 }')
    fi

    (("${#active_names[@]}" <= 1)) ||
        fail "Несколько UART unit активны; установщик не будет выбирать устройство."
    if (("${#active_names[@]}" == 1)); then
        ACTIVE_UART="${active_names[0]}"
    fi

    if "$SYSTEMCTL" is-active --quiet dockstation-gateway.service; then
        BASE_ACTIVE=true
    fi

    [[ -z "$ACTIVE_UART" || "$BASE_ACTIVE" != true ]] ||
        fail "Одновременно активны базовый и UART unit; устраните конфликт вручную."

    if [[ "$CONFIG_UART" == true && "$BASE_ACTIVE" == true && -z "$ACTIVE_UART" ]]; then
        fail "controller_uart_device задан при активном базовом unit без доступа к физическому /dev."
    fi

    if [[ "$CONFIG_UART" == true && -n "$ACTIVE_UART" ]]; then
        configured_path="$(sed -n 's/^[[:space:]]*controller_uart_device[[:space:]]*=[[:space:]]*//p' "$CONFIG_FILE" | head -n 1 | tr -d '"')"
        configured_name="$(basename -- "$configured_path")"
        resolved_path="$(readlink -f -- "$configured_path" 2>/dev/null || printf '%s' "$configured_path")"
        resolved_name="$(basename -- "$resolved_path")"
        active_instance="${ACTIVE_UART#dockstation-gateway-uart@}"
        active_instance="${active_instance%.service}"
        [[ "$configured_name" == "$active_instance" || "$resolved_name" == "$active_instance" ]] ||
            fail "Настроенный UART ($configured_path) не совпадает с активным systemd instance ($active_instance)."
    fi
fi

BUNDLE_TEMP=""
if [[ -n "$BUNDLE" ]]; then
    BUNDLE_TEMP="$(mktemp -d)"
    trap 'rm -rf "$BUNDLE_TEMP"' EXIT
    # The same immutable snapshot was hashed and passed to the verifier above.
    tar --no-same-owner --no-same-permissions -xzf "$BUNDLE_SNAPSHOT" -C "$BUNDLE_TEMP"
    BUNDLE_PREFIX="$BUNDLE_TEMP$PREFIX"
    [[ -x "$BUNDLE_PREFIX/bin/dockstation_linux_bootstrap" ]] ||
        fail "Bundle не содержит исполняемый Gateway."
    [[ -x "$BUNDLE_PREFIX/bin/dockstation-credential-provision" ]] ||
        fail "Bundle не содержит credential provisioner."
    [[ -f "$BUNDLE_PREFIX/lib/systemd/system/dockstation-gateway.service" ]] ||
        fail "Bundle не содержит базовый systemd unit."
    [[ -f "$BUNDLE_PREFIX/lib/systemd/system/dockstation-gateway-uart@.service" ]] ||
        fail "Bundle не содержит UART systemd template."
    [[ -f "$BUNDLE_PREFIX/share/dockstation/gateway.conf.example" ]] ||
        fail "Bundle не содержит config example."

    info "Установка готового ARM64 bundle без CMake и компилятора."
    "$INSTALL" -D -m 0755 "$BUNDLE_PREFIX/bin/dockstation_linux_bootstrap" \
        "$STAGE_ROOT$PREFIX/bin/dockstation_linux_bootstrap"
    "$INSTALL" -D -m 0755 "$BUNDLE_PREFIX/bin/dockstation-credential-provision" \
        "$STAGE_ROOT$PREFIX/bin/dockstation-credential-provision"
    "$INSTALL" -D -m 0644 "$BUNDLE_PREFIX/lib/systemd/system/dockstation-gateway.service" \
        "$STAGE_ROOT$BASE_UNIT"
    "$INSTALL" -D -m 0644 "$BUNDLE_PREFIX/lib/systemd/system/dockstation-gateway-uart@.service" \
        "$STAGE_ROOT$UART_UNIT"
    "$INSTALL" -D -m 0644 "$BUNDLE_PREFIX/share/dockstation/gateway.conf.example" \
        "$STAGE_ROOT$EXAMPLE_CONFIG"
else
    info "Сборка Release Gateway без SQLite development dependency."
    "$CMAKE" -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DDOCKSTATION_ENABLE_SANITIZERS=OFF \
        -DDOCKSTATION_BUILD_SQLITE_PERSISTENCE=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=TRUE \
        -DCMAKE_INSTALL_PREFIX="$PREFIX"

    "$CMAKE" --build "$BUILD_DIR" \
        --target dockstation_linux_bootstrap dockstation-credential-provision \
        --parallel "$BUILD_JOBS"
    "$CMAKE" --build "$BUILD_DIR" --target preinstall --parallel "$BUILD_JOBS"
    DESTDIR="$STAGE_ROOT" "$CMAKE" --install "$BUILD_DIR"
fi

BIN="$STAGE_ROOT$PREFIX/bin/dockstation_linux_bootstrap"
PROVISIONER="$STAGE_ROOT$PREFIX/bin/dockstation-credential-provision"
BASE_FILE="$STAGE_ROOT$BASE_UNIT"
UART_FILE="$STAGE_ROOT$UART_UNIT"
EXAMPLE_FILE="$STAGE_ROOT$EXAMPLE_CONFIG"
[[ -x "$BIN" && -x "$PROVISIONER" ]] ||
    fail "Не все исполняемые Gateway файлы установлены."
[[ -f "$BASE_FILE" && -f "$UART_FILE" && -f "$EXAMPLE_FILE" ]] ||
    fail "Не все ожидаемые systemd/config files установлены."

if [[ -n "$STAGE_ROOT" ]]; then
    "$INSTALL" -d -m 0750 "$TARGET_CONFIG_DIR"
    [[ -e "$TARGET_CONFIG_FILE" ]] ||
        "$INSTALL" -m 0640 "$EXAMPLE_FILE" "$TARGET_CONFIG_FILE"
    # In staging mode ExecStart paths intentionally do not exist on the host.
    # Target systemd verification runs after installing files into real /usr/local.
    info "Staging завершён; учётные записи и systemd manager хоста не затрагивались."
    exit 0
fi

"$GETENT" group dockstation >/dev/null 2>&1 || "$GROUPADD" --system dockstation
"$GETENT" passwd dockstation >/dev/null 2>&1 ||
    "$USERADD" --system --gid dockstation --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin dockstation

"$INSTALL" -d -o root -g dockstation -m 0750 "$CONFIG_DIR"
if [[ ! -e "$CONFIG_FILE" ]]; then
    "$INSTALL" -o root -g dockstation -m 0640 "$EXAMPLE_FILE" "$CONFIG_FILE"
    info "Создана начальная конфигурация."
else
    "$CHOWN" root:dockstation "$CONFIG_FILE"
    "$CHMOD" 0640 "$CONFIG_FILE"
    info "Существующая конфигурация сохранена."
fi

"$SYSTEMD_ANALYZE" verify "$BASE_FILE" "$UART_FILE"
"$SYSTEMCTL" daemon-reload

if [[ -n "$ACTIVE_UART" ]]; then
    "$SYSTEMCTL" restart "$ACTIVE_UART"
    "$SYSTEMCTL" is-active --quiet "$ACTIVE_UART" ||
        fail "UART unit $ACTIVE_UART не вернулся в active после обновления."
    info "Существующий UART profile обновлён без изменения аппаратного режима."
    exit 0
fi

if [[ "$CONFIG_UART" == true ]]; then
    info "controller_uart_device задан, но UART unit не активен; базовый unit не запускается."
    info "После аппаратной проверки включите адресный dockstation-gateway-uart@<device>.service."
    exit 0
fi

if [[ "$BASE_ACTIVE" == true ]]; then
    "$SYSTEMCTL" restart dockstation-gateway.service
else
    "$SYSTEMCTL" enable --now dockstation-gateway.service
fi
"$SYSTEMCTL" is-active --quiet dockstation-gateway.service ||
    fail "dockstation-gateway.service не перешёл в active."

if command -v curl >/dev/null 2>&1; then
    health_response="$(curl --fail --silent --show-error --max-time 5 \
        http://127.0.0.1:8080/api/v1/health)" ||
        fail "Gateway запущен, но локальный health endpoint не ответил."
    [[ "$health_response" == *'"status":"PASS"'* ]] ||
        fail "Health endpoint вернул неожиданный ответ."
    info "Локальный Gateway health endpoint: PASS."
else
    warn "curl отсутствует; проверен только systemd active-state."
fi

info "Gateway установлен и запущен как dockstation:dockstation."
info "Базовый профиль сохраняет PrivateDevices=true; UART/GPIO/камера не включались автоматически."
