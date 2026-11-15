#!/usr/bin/env bash
set -euo pipefail

GETENT="${DOCKSTATION_GETENT_COMMAND:-getent}"
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '[INFO] %s\n' "$*" ; }

lookup_record() {
    local database="$1"
    local key="$2"
    local output status

    if output="$("$GETENT" "$database" "$key" 2>/dev/null)"; then
        if [[ -z "$output" || "$output" == *$'\n'* ]]; then
            printf 'ERROR: getent вернул пустую или неоднозначную запись для %s %s.\n' "$database" "$key" >&2
            return 2
        fi
        printf '%s\n' "$output"
        return 0
    else
        status=$?
        if [[ "$status" -eq 2 ]]; then return 1; fi
        printf 'ERROR: Не удалось проверить запись %s %s (код getent=%s).\n' "$database" "$key" "$status" >&2
        return 2
    fi
}

group_record=""
user_record=""
group_exists=false
user_exists=false
if group_record="$(lookup_record group dockstation)"; then
    group_exists=true
else
    lookup_status=$?
    [[ "$lookup_status" -eq 1 ]] || exit "$lookup_status"
fi
if user_record="$(lookup_record passwd dockstation)"; then
    user_exists=true
else
    lookup_status=$?
    [[ "$lookup_status" -eq 1 ]] || exit "$lookup_status"
fi

if [[ "$group_exists" == true ]]; then
    IFS=: read -r -a group_fields <<< "$group_record"
    (( ${#group_fields[@]} >= 3 && ${#group_fields[@]} <= 4 )) || fail "Запись группы dockstation имеет неожиданный формат."
    group_name="${group_fields[0]}"
    group_gid="${group_fields[2]}"
    [[ "$group_name" == dockstation && "$group_gid" =~ ^[0-9]+$ ]] ||
        fail "Запись группы dockstation имеет неожиданный формат."
    (( 10#$group_gid > 0 )) || fail "Группа dockstation не должна иметь GID 0 (root)."
else
    group_gid=""
fi

if [[ "$user_exists" == true ]]; then
    [[ "$group_exists" == true ]] ||
        fail "Пользователь dockstation существует, но его одноимённая группа отсутствует."
    IFS=: read -r -a user_fields <<< "$user_record"
    (( ${#user_fields[@]} == 7 )) || fail "Запись пользователя dockstation имеет неожиданный формат."
    user_name="${user_fields[0]}"
    user_uid="${user_fields[2]}"
    user_gid="${user_fields[3]}"
    user_home="${user_fields[5]}"
    user_shell="${user_fields[6]}"
    [[ "$user_name" == dockstation && "$user_uid" =~ ^[0-9]+$ && "$user_gid" =~ ^[0-9]+$ ]] ||
        fail "Запись пользователя dockstation имеет неожиданный формат."
    (( 10#$user_uid > 0 )) || fail "Пользователь dockstation не должен иметь UID 0 (root)."
    [[ "$user_gid" == "$group_gid" ]] ||
        fail "Primary GID пользователя dockstation ($user_gid) не совпадает с GID группы ($group_gid)."
    [[ "$user_home" == /nonexistent ]] ||
        fail "Домашний каталог пользователя dockstation должен быть /nonexistent (получено: $user_home)."
    case "$user_shell" in
        /usr/sbin/nologin|/sbin/nologin) ;;
        *) fail "Пользователь dockstation должен использовать nologin shell (получено: $user_shell)." ;;
    esac
fi

if [[ "$group_exists" == true && "$user_exists" == true ]]; then
    info "Существующие group/user dockstation имеют непривилегированные UID/GID и ожидаемые параметры."
elif [[ "$group_exists" == true ]]; then
    info "Существующая группа dockstation безопасна; пользователь будет создан установщиком."
elif [[ "$user_exists" == true ]]; then
    fail "Невозможно продолжить: пользователь dockstation найден без группы."
else
    info "Учётные записи dockstation отсутствуют; установщик создаст их как системные."
fi
