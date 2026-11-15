#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PREFLIGHT="${DOCKSTATION_PREFLIGHT_SCRIPT:-$ROOT/repka/check_target_platform.sh}"
CMAKE="${DOCKSTATION_CMAKE_COMMAND:-cmake}"
SYSTEMCTL="${DOCKSTATION_SYSTEMCTL_COMMAND:-systemctl}"
SYSTEMD_ANALYZE="${DOCKSTATION_SYSTEMD_ANALYZE_COMMAND:-systemd-analyze}"
GETENT="${DOCKSTATION_GETENT_COMMAND:-getent}"
GROUPADD="${DOCKSTATION_GROUPADD_COMMAND:-groupadd}"
USERADD="${DOCKSTATION_USERADD_COMMAND:-useradd}"
INSTALL="${DOCKSTATION_INSTALL_COMMAND:-install}"
CHOWN="${DOCKSTATION_CHOWN_COMMAND:-chown}"
CHMOD="${DOCKSTATION_CHMOD_COMMAND:-chmod}"
BUILD_DIR="${DOCKSTATION_BUILD_DIR:-$ROOT/build}"
STAGE="${DOCKSTATION_INSTALL_ROOT:-}"
JOBS="${DOCKSTATION_BUILD_JOBS:-2}"
PREFIX=/usr/local
CFG_DIR=/etc/dockstation
CFG_FILE=$CFG_DIR/gateway.conf
BASE_UNIT=$PREFIX/lib/systemd/system/dockstation-gateway.service
UART_UNIT=$PREFIX/lib/systemd/system/dockstation-gateway-uart@.service
EXAMPLE=$PREFIX/share/dockstation/gateway.conf.example
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*" >&2; }

[[ -n "$STAGE" || "$EUID" -eq 0 ]] || fail "Запустите от root: sudo bash repka/install_gateway.sh"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "DOCKSTATION_BUILD_JOBS должен быть положительным целым числом."
[[ -f "$PREFLIGHT" ]] || fail "Не найден preflight: $PREFLIGHT"

# Exit 1 is warning-only (e.g. no verified UART); exit 2 is a hard blocker.
preflight_status=0
if "$PREFLIGHT"; then preflight_status=0; else preflight_status=$?; fi
((preflight_status <= 1)) || fail "Preflight выявил обязательную проблему (код $preflight_status)."
[[ "$preflight_status" -eq 0 ]] || warn "Есть предупреждения preflight; аппаратный UART не включается."

for cmd in "$CMAKE" "$SYSTEMD_ANALYZE"; do
    command -v "$cmd" >/dev/null 2>&1 || fail "Не найдена команда $cmd"
done

ACTIVE_UART=""
BASE_ACTIVE=false
CFG_UART=false
if [[ -z "$STAGE" ]]; then
    for cmd in "$SYSTEMCTL" "$GETENT" "$GROUPADD" "$USERADD"; do
        command -v "$cmd" >/dev/null 2>&1 || fail "Не найдена команда $cmd"
    done
    if [[ -f "$CFG_FILE" ]] && grep -Eq '^[[:space:]]*controller_uart_device[[:space:]]*=' "$CFG_FILE"; then
        CFG_UART=true
    fi
    active_list="$("$SYSTEMCTL" list-units --type=service --state=active --no-legend --plain \
        'dockstation-gateway-uart@*.service' 2>/dev/null || true)"
    active_names=()
    if [[ -n "$active_list" ]]; then
        while IFS= read -r n; do [[ -n "$n" ]] && active_names+=("$n"); done \
            < <(printf '%s\n' "$active_list" | awk 'NF {print $1}')
    fi
    (("${#active_names[@]}" <= 1)) || fail "Несколько UART unit активны; установщик не выбирает устройство."
    (("${#active_names[@]}" == 0)) || ACTIVE_UART="${active_names[0]}"
    if "$SYSTEMCTL" is-active --quiet dockstation-gateway.service; then BASE_ACTIVE=true; fi
    [[ -z "$ACTIVE_UART" || "$BASE_ACTIVE" != true ]] ||
        fail "Одновременно активны базовый и UART unit; устраните конфликт вручную."
    if [[ "$CFG_UART" == true && "$BASE_ACTIVE" == true && -z "$ACTIVE_UART" ]]; then
        fail "controller_uart_device задан при активном базовом unit без доступа к физическому /dev."
    fi
fi

info "Сборка Release Gateway без SQLite development dependency."
"$CMAKE" -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DDOCKSTATION_ENABLE_SANITIZERS=OFF \
    -DDOCKSTATION_BUILD_SQLITE_PERSISTENCE=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=TRUE \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
"$CMAKE" --build "$BUILD_DIR" --target dockstation_linux_bootstrap dockstation-credential-provision --parallel "$JOBS"
"$CMAKE" --build "$BUILD_DIR" --target preinstall --parallel "$JOBS"
DESTDIR="$STAGE" "$CMAKE" --install "$BUILD_DIR"

BIN="$STAGE$PREFIX/bin/dockstation_linux_bootstrap"
BASE_FILE="$STAGE$BASE_UNIT"
UART_FILE="$STAGE$UART_UNIT"
EXAMPLE_FILE="$STAGE$EXAMPLE"
[[ -x "$BIN" ]] || fail "Не найден исполняемый Gateway: $BIN"
[[ -f "$BASE_FILE" && -f "$UART_FILE" && -f "$EXAMPLE_FILE" ]] || fail "Не хватает установленных unit/config files."

if [[ -n "$STAGE" ]]; then
    "$INSTALL" -d -m 0750 "$STAGE$CFG_DIR"
    [[ -e "$STAGE$CFG_FILE" ]] || "$INSTALL" -m 0640 "$EXAMPLE_FILE" "$STAGE$CFG_FILE"
    "$SYSTEMD_ANALYZE" verify "$BASE_FILE" "$UART_FILE"
    info "Staging завершён; accounts и systemd manager хоста не затронуты."
    exit 0
fi

"$GETENT" group dockstation >/dev/null 2>&1 || "$GROUPADD" --system dockstation
"$GETENT" passwd dockstation >/dev/null 2>&1 ||
    "$USERADD" --system --gid dockstation --no-create-home --home-dir /nonexistent \
        --shell /usr/sbin/nologin dockstation
[[ ! -L "$CFG_DIR" ]] || fail "$CFG_DIR является symlink."
"$INSTALL" -d -o root -g dockstation -m 0750 "$CFG_DIR"
[[ ! -L "$CFG_FILE" ]] || fail "$CFG_FILE является symlink."
if [[ ! -e "$CFG_FILE" ]]; then
    "$INSTALL" -o root -g dockstation -m 0640 "$EXAMPLE" "$CFG_FILE"
else
    "$CHOWN" root:dockstation "$CFG_FILE"
    "$CHMOD" 0640 "$CFG_FILE"
fi

"$SYSTEMD_ANALYZE" verify "$BASE_UNIT" "$UART_UNIT"
"$SYSTEMCTL" daemon-reload
if [[ -n "$ACTIVE_UART" ]]; then
    "$SYSTEMCTL" restart "$ACTIVE_UART"
    "$SYSTEMCTL" is-active --quiet "$ACTIVE_UART" || fail "UART unit $ACTIVE_UART не вернулся в active."
    info "Существующий UART instance обновлён без изменения аппаратного режима."
    exit 0
fi
if [[ "$CFG_UART" == true ]]; then
    info "Файлы установлены; controller_uart_device задан, но UART unit не активен. Gateway не запускается."
    exit 0
fi
if [[ "$BASE_ACTIVE" == true ]]; then
    "$SYSTEMCTL" restart dockstation-gateway.service
else
    "$SYSTEMCTL" enable --now dockstation-gateway.service
fi
"$SYSTEMCTL" is-active --quiet dockstation-gateway.service || fail "Базовая служба не перешла в active."
if command -v curl >/dev/null 2>&1; then
    response="$(curl --fail --silent --show-error --max-time 5 http://127.0.0.1:8080/api/v1/health)" ||
        fail "Gateway не ответил на health request."
    [[ "$response" == *'"status":"PASS"'* ]] || fail "Health endpoint вернул неожиданный ответ."
else
    warn "curl отсутствует; проверен только systemd active-state."
fi
info "Gateway установлен и запущен как dockstation:dockstation; физические устройства не открывались."
