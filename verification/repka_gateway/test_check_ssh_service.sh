#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
SCRIPT_PATH="$REPOSITORY_ROOT/repka/check_network.sh"

if [[ ! -f "$SCRIPT_PATH" ]]; then
    printf 'FAIL: не найден скрипт: %s\n' "$SCRIPT_PATH" >&2
    exit 1
fi

source "$SCRIPT_PATH"

TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT
FAKE_SYSTEMCTL="$TEMP_ROOT/systemctl"

cat > "$FAKE_SYSTEMCTL" <<'FAKE_SYSTEMCTL_SCRIPT'
#!/usr/bin/env bash

if [[ "$#" -ne 4 || "$1" != "show" || "$2" != "--no-pager" ||
    "$3" != "--property=LoadState,ActiveState,SubState,UnitFileState" ]]; then
    printf 'Unexpected systemctl arguments: %s\n' "$*" >&2
    exit 64
fi
unit="$4"

not_found() {
    printf '%s\n' 'LoadState=not-found' 'ActiveState=' 'SubState=' 'UnitFileState='
}

case "${FAKE_SYSTEMCTL_MODE:-}" in
    active-ssh-service)
        if [[ "$unit" == "ssh.service" ]]; then
            printf '%s\n' 'LoadState=loaded' 'ActiveState=active' 'SubState=running' 'UnitFileState=enabled'
        else
            not_found
        fi
        ;;
    active-sshd-service)
        if [[ "$unit" == "sshd.service" ]]; then
            printf '%s\n' 'LoadState=loaded' 'ActiveState=active' 'SubState=running' 'UnitFileState=enabled'
        else
            not_found
        fi
        ;;
    active-ssh-socket)
        if [[ "$unit" == "ssh.socket" ]]; then
            printf '%s\n' 'LoadState=loaded' 'ActiveState=active' 'SubState=listening' 'UnitFileState=enabled'
        else
            not_found
        fi
        ;;
    active-sshd-socket)
        if [[ "$unit" == "sshd.socket" ]]; then
            printf '%s\n' 'LoadState=loaded' 'ActiveState=active' 'SubState=listening' 'UnitFileState=static'
        else
            not_found
        fi
        ;;
    stopped)
        if [[ "$unit" == "ssh.service" ]]; then
            printf '%s\n' 'LoadState=loaded' 'ActiveState=inactive' 'SubState=dead' 'UnitFileState=enabled'
        else
            not_found
        fi
        ;;
    failed)
        if [[ "$unit" == "ssh.service" ]]; then
            printf '%s\n' 'LoadState=loaded' 'ActiveState=failed' 'SubState=failed' 'UnitFileState=enabled'
        else
            not_found
        fi
        ;;
    masked)
        if [[ "$unit" == "ssh.service" ]]; then
            printf '%s\n' 'LoadState=masked' 'ActiveState=inactive' 'SubState=dead' 'UnitFileState=masked'
        else
            not_found
        fi
        ;;
    all-missing)
        not_found
        ;;
    systemd-unavailable)
        printf '%s\n' 'System has not been booted with systemd as init system (PID 1). Can'\''t operate.' >&2
        exit 1
        ;;
    generic-error)
        printf '%s\n' 'simulated systemctl failure' >&2
        exit 7
        ;;
    malformed)
        printf '%s\n' 'LoadState=loaded' 'SubState=running' 'UnitFileState=enabled'
        ;;
    active-with-partial-error)
        if [[ "$unit" == "ssh.service" ]]; then
            printf '%s\n' 'LoadState=loaded' 'ActiveState=active' 'SubState=running' 'UnitFileState=enabled'
        elif [[ "$unit" == "sshd.service" ]]; then
            printf '%s\n' 'simulated read error' >&2
            exit 7
        else
            not_found
        fi
        ;;
    *)
        printf 'Unknown fake mode: %s\n' "${FAKE_SYSTEMCTL_MODE:-<empty>}" >&2
        exit 65
        ;;
esac
FAKE_SYSTEMCTL_SCRIPT
chmod +x "$FAKE_SYSTEMCTL"

run_case() {
    local description="$1"
    local expected_status="$2"
    local expected_text="$3"
    shift 3
    local output
    local actual_status

    if output="$("$@" 2>&1)"; then
        actual_status=0
    else
        actual_status=$?
    fi

    if [[ "$actual_status" -ne "$expected_status" ]]; then
        printf 'FAIL: %s: ожидался код %s, получен %s\n%s\n' "$description" "$expected_status" "$actual_status" "$output" >&2
        exit 1
    fi

    if [[ "$output" != *"$expected_text"* ]]; then
        printf 'FAIL: %s: в выводе отсутствует "%s"\n%s\n' "$description" "$expected_text" "$output" >&2
        exit 1
    fi

    printf 'PASS: %s\n' "$description"
}

export FAKE_SYSTEMCTL_MODE="active-ssh-service"
run_case "активная служба ssh.service" 0 "служба активна" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="active-sshd-service"
run_case "активная служба sshd.service" 0 "служба активна" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="active-ssh-socket"
run_case "активный ssh.socket (socket activation)" 0 "socket activation" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="active-sshd-socket"
run_case "активный sshd.socket (socket activation)" 0 "socket activation" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="stopped"
run_case "остановленная SSH-служба" 1 "остановлена" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="failed"
run_case "аварийно завершившаяся SSH-служба" 1 "завершилась с ошибкой" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="masked"
run_case "замаскированная SSH-служба" 1 "замаскированы" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="all-missing"
run_case "отсутствуют все SSH units" 2 "не обнаружены" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="systemd-unavailable"
run_case "менеджер systemd недоступен" 1 "systemd недоступен" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="generic-error"
run_case "ошибка systemctl при чтении unit" 1 "не удалось получить состояние" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="malformed"
run_case "отсутствует ActiveState в выводе systemctl" 1 "не вернул ActiveState" check_ssh_service "$FAKE_SYSTEMCTL"

export FAKE_SYSTEMCTL_MODE="active-with-partial-error"
run_case "активная служба при частичной ошибке диагностики" 1 "часть состояний не удалось проверить" check_ssh_service "$FAKE_SYSTEMCTL"

run_case "команда systemctl отсутствует" 2 "не найдена" check_ssh_service "$TEMP_ROOT/missing-systemctl"

printf '\nВсе проверки check_ssh_service пройдены.\n'
