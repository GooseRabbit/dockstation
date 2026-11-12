#!/usr/bin/env bash

# Shared OS/architecture preflight and final-report helpers.
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/network_helpers.sh"

# Read-only network diagnostics for the Repka/Linux deployment.
# The functions are read-only and can be sourced for focused host verification.

check_ethernet_link() {
    local sys_class_net_root="${1:-/sys/class/net}"
    local interface_path
    local interface_name
    local hardware_type
    local carrier
    local ethernet_count=0
    local linked_count=0
    local unknown_count=0

    if [[ ! -d "$sys_class_net_root" ]]; then
        printf '[FAIL] Ethernet: каталог сетевых интерфейсов недоступен: %s\n' \
            "$sys_class_net_root"
        return 2
    fi

    # A real Ethernet device (including a VM's virtual NIC) has ARPHRD_ETHER
    # type 1 and a sysfs device entry. Wireless, bridge and loopback devices
    # are deliberately excluded. Interface names are discovered, not assumed.
    for interface_path in "$sys_class_net_root"/*; do
        [[ -d "$interface_path" ]] || continue

        interface_name="${interface_path##*/}"
        [[ "$interface_name" != "lo" ]] || continue
        [[ ! -d "$interface_path/wireless" ]] || continue
        [[ ! -d "$interface_path/bridge" ]] || continue
        [[ -e "$interface_path/device" ]] || continue

        if [[ ! -r "$interface_path/type" ]] ||
            ! IFS= read -r hardware_type < "$interface_path/type"; then
            printf '[UNKNOWN] Ethernet %s: невозможно прочитать тип интерфейса.\n' \
                "$interface_name"
            ((unknown_count += 1))
            continue
        fi

        [[ "$hardware_type" == "1" ]] || continue
        ((ethernet_count += 1))

        if [[ ! -r "$interface_path/carrier" ]] ||
            ! IFS= read -r carrier < "$interface_path/carrier"; then
            printf '[UNKNOWN] Ethernet %s: состояние физического линка недоступно.\n' \
                "$interface_name"
            ((unknown_count += 1))
            continue
        fi

        case "$carrier" in
            1)
                printf '[PASS] Ethernet %s: физический линк активен.\n' \
                    "$interface_name"
                ((linked_count += 1))
                ;;
            0)
                printf '[WARN] Ethernet %s: физический линк не обнаружен.\n' \
                    "$interface_name"
                ;;
            *)
                printf '[UNKNOWN] Ethernet %s: неожиданный статус carrier: %s.\n' \
                    "$interface_name" "$carrier"
                ((unknown_count += 1))
                ;;
        esac
    done

    if ((ethernet_count == 0)); then
        if ((unknown_count > 0)); then
            printf '[WARN] Ethernet: не удалось достоверно определить доступные интерфейсы.\n'
            return 1
        fi

        printf '[FAIL] Ethernet: аппаратные Ethernet-интерфейсы не обнаружены.\n'
        return 2
    fi

    if ((linked_count > 0)); then
        printf '[PASS] Ethernet: физический линк активен как минимум на одном интерфейсе (%d из %d).\n' \
            "$linked_count" "$ethernet_count"
        return 0
    fi

    if ((unknown_count > 0)); then
        printf '[WARN] Ethernet: интерфейсы обнаружены, но состояние физического линка определено не полностью.\n'
        return 1
    fi

    printf '[WARN] Ethernet: интерфейсы обнаружены (%d), но физический линк отсутствует.\n' \
        "$ethernet_count"
    return 1
}


# Read assigned IPv4/IPv6 addresses from all interfaces without changing
# network configuration. The optional command path makes the check testable.
read_ip_configuration() {
    local ip_command="${1:-ip}"
    local ip_output
    local command_status
    local line
    local interface_index
    local interface_name
    local address_family
    local local_address
    local remainder
    local address_count=0
    local unknown_count=0

    if ! command -v "$ip_command" >/dev/null 2>&1; then
        printf '[FAIL] IP: команда "%s" не найдена.\n' "$ip_command"
        return 2
    fi

    if ip_output="$("$ip_command" -o address show 2>&1)"; then
        :
    else
        command_status=$?
        printf '[UNKNOWN] IP: команда "%s" завершилась с кодом %d' \
            "$ip_command" "$command_status"
        if [[ -n "$ip_output" ]]; then
            printf ': %s' "$ip_output"
        fi
        printf '\n'
        return 1
    fi

    if [[ -z "$ip_output" ]]; then
        printf '[WARN] IP: команда не вернула назначенных адресов.\n'
        return 1
    fi

    while IFS= read -r line; do
        [[ -n "$line" ]] || continue

        interface_index=""
        interface_name=""
        address_family=""
        local_address=""
        remainder=""
        read -r interface_index interface_name address_family local_address remainder <<< "$line"

        if [[ ! "$interface_index" =~ ^[0-9]+:$ ]] ||
            [[ -z "$interface_name" ]] ||
            [[ "$address_family" != "inet" && "$address_family" != "inet6" ]] ||
            [[ "$local_address" != */* ]]; then
            printf '[UNKNOWN] IP: не удалось разобрать строку: %s\n' "$line"
            ((unknown_count += 1))
            continue
        fi

        case "$address_family" in
            inet) address_family="IPv4" ;;
            inet6) address_family="IPv6" ;;
        esac

        printf '[PASS] IP %s (%s): %s\n' \
            "$interface_name" "$address_family" "$local_address"
        if [[ -n "$remainder" ]]; then
            printf '[INFO] IP %s metadata: %s\n' "$interface_name" "$remainder"
        fi
        ((address_count += 1))
    done <<< "$ip_output"

    if ((address_count == 0)); then
        printf '[WARN] IP: не найдено корректных назначенных IPv4/IPv6 адресов.\n'
        return 1
    fi

    if ((unknown_count > 0)); then
        printf '[WARN] IP: прочитано адресов: %d; строк с неизвестным форматом: %d.\n' \
            "$address_count" "$unknown_count"
        return 1
    fi

    printf '[PASS] IP: прочитано назначенных адресов: %d.\n' "$address_count"
    return 0
}

# Check IPv4 and IPv6 default routes without changing routing configuration.
# A missing route is a warning; command and output errors remain distinguishable.
check_default_route() {
    local ip_command="${1:-ip}"
    local ipv4_output=""
    local ipv6_output=""
    local ipv4_status=0
    local ipv6_status=0
    local line
    local route_kind
    local route_count=0
    local unknown_count=0

    if ! command -v "$ip_command" >/dev/null 2>&1; then
        printf '[FAIL] Маршрут: команда "%s" не найдена.\n' "$ip_command"
        return 2
    fi

    if ipv4_output="$("$ip_command" -4 -o route show default 2>&1)"; then
        ipv4_status=0
    else
        ipv4_status=$?
    fi

    if ipv6_output="$("$ip_command" -6 -o route show default 2>&1)"; then
        ipv6_status=0
    else
        ipv6_status=$?
    fi

    if ((ipv4_status != 0)); then
        printf '[UNKNOWN] Маршрут IPv4: команда ip завершилась с кодом %d' "$ipv4_status"
        if [[ -n "$ipv4_output" ]]; then
            printf ': %s' "$ipv4_output"
        fi
        printf '\n'
        ((unknown_count += 1))
    elif [[ -n "$ipv4_output" ]]; then
        while IFS= read -r line; do
            [[ -n "$line" ]] || continue
            route_kind="${line%%[[:space:]]*}"
            if [[ "$route_kind" != "default" ]]; then
                printf '[UNKNOWN] Маршрут IPv4: неожиданный формат строки: %s\n' "$line"
                ((unknown_count += 1))
                continue
            fi
            printf '[PASS] Маршрут по умолчанию IPv4: %s\n' "$line"
            ((route_count += 1))
        done <<< "$ipv4_output"
    fi

    if ((ipv6_status != 0)); then
        printf '[UNKNOWN] Маршрут IPv6: команда ip завершилась с кодом %d' "$ipv6_status"
        if [[ -n "$ipv6_output" ]]; then
            printf ': %s' "$ipv6_output"
        fi
        printf '\n'
        ((unknown_count += 1))
    elif [[ -n "$ipv6_output" ]]; then
        while IFS= read -r line; do
            [[ -n "$line" ]] || continue
            route_kind="${line%%[[:space:]]*}"
            if [[ "$route_kind" != "default" ]]; then
                printf '[UNKNOWN] Маршрут IPv6: неожиданный формат строки: %s\n' "$line"
                ((unknown_count += 1))
                continue
            fi
            printf '[PASS] Маршрут по умолчанию IPv6: %s\n' "$line"
            ((route_count += 1))
        done <<< "$ipv6_output"
    fi

    if ((unknown_count > 0)); then
        if ((route_count > 0)); then
            printf '[WARN] Маршрут: обнаружено маршрутов по умолчанию: %d; проверка завершена не полностью.\n' \
                "$route_count"
        else
            printf '[WARN] Маршрут: не удалось достоверно определить наличие маршрута по умолчанию.\n'
        fi
        return 1
    fi

    if ((route_count == 0)); then
        printf '[WARN] Маршрут: маршруты по умолчанию IPv4/IPv6 не обнаружены.\n'
        return 1
    fi

    printf '[PASS] Маршрут: обнаружено маршрутов по умолчанию: %d.\n' "$route_count"
    return 0
}

# Check name resolution through the operating system's NSS resolver.
# The target is supplied explicitly so diagnostics do not assume public Internet access.
check_dns_resolution() {
    local dns_name="${1:-${DOCKSTATION_DNS_CHECK_NAME:-}}"
    local timeout_seconds="${2:-5}"
    local dns_name_pattern='^([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(\.([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?))*\.?$'
    local normalized_length="${#dns_name}"
    local resolution_output=""
    local command_status=0

    if [[ -z "$dns_name" ]]; then
        printf '[WARN] DNS: контрольное имя не задано; установите DOCKSTATION_DNS_CHECK_NAME.\n'
        return 1
    fi

    if [[ "$dns_name" == *"." ]]; then
        normalized_length=$((normalized_length - 1))
    fi

    if ((normalized_length < 1 || normalized_length > 253)) ||
        [[ ! "$dns_name" =~ $dns_name_pattern ]]; then
        printf '[FAIL] DNS: некорректное контрольное имя: %s.\n' "$dns_name"
        return 2
    fi

    if [[ ! "$timeout_seconds" =~ ^([1-9]|[1-5][0-9]|60)$ ]]; then
        printf '[FAIL] DNS: тайм-аут должен быть целым числом от 1 до 60 секунд.\n'
        return 2
    fi

    if ! command -v getent >/dev/null 2>&1; then
        printf '[FAIL] DNS: команда getent не найдена.\n'
        return 2
    fi

    if ! command -v timeout >/dev/null 2>&1; then
        printf '[FAIL] DNS: команда timeout не найдена; проверку нельзя выполнить с ограничением времени.\n'
        return 2
    fi

    if resolution_output="$(timeout --signal=TERM --kill-after=1s "${timeout_seconds}s" getent ahosts "$dns_name" 2>&1)"; then
        if [[ -z "$resolution_output" ]]; then
            printf '[WARN] DNS: команда getent не вернула адресов для имени "%s".\n' "$dns_name"
            return 1
        fi

        printf '[PASS] DNS: имя "%s" разрешено системным механизмом имён.\n%s\n' \
            "$dns_name" "$resolution_output"
        return 0
    else
        command_status=$?
    fi

    if ((command_status == 124 || command_status == 137)); then
        printf '[WARN] DNS: проверка имени "%s" превысила тайм-аут %s с.\n' \
            "$dns_name" "$timeout_seconds"
        return 1
    fi

    printf '[WARN] DNS: не удалось разрешить имя "%s" (код getent/timeout: %d)' \
        "$dns_name" "$command_status"
    if [[ -n "$resolution_output" ]]; then
        printf ': %s' "$resolution_output"
    fi
    printf '\n'
    return 1
}

# Check SSH service units using systemd's read-only property interface.
# Both service names and socket activation are recognized; this function never changes units.
check_ssh_service() {
    local systemctl_command="${1:-systemctl}"
    local unit
    local unit_output
    local command_status
    local property
    local value
    local load_state
    local active_state
    local sub_state
    local unit_file_state
    local unit_kind
    local service_units_found=0
    local socket_units_found=0
    local active_services=0
    local active_sockets=0
    local unknown_count=0
    local masked_count=0

    if ! command -v "$systemctl_command" >/dev/null 2>&1; then
        printf '[FAIL] SSH: команда "%s" не найдена.\n' "$systemctl_command"
        return 2
    fi

    for unit in ssh.service sshd.service ssh.socket sshd.socket; do
        unit_output=""
        load_state=""
        active_state=""
        sub_state=""
        unit_file_state=""

        if unit_output="$("$systemctl_command" show --no-pager --property=LoadState,ActiveState,SubState,UnitFileState "$unit" 2>&1)"; then
            command_status=0
        else
            command_status=$?
        fi

        if [[ "$unit_output" == *"System has not been booted with systemd"* ]] ||
            [[ "$unit_output" == *"Failed to connect to bus"* ]] ||
            [[ "$unit_output" == *"Failed to get D-Bus connection"* ]] ||
            [[ "$unit_output" == *"Host is down"* ]]; then
            printf '[WARN] SSH: менеджер systemd недоступен; состояние службы определить нельзя.\n'
            return 1
        fi

        while IFS='=' read -r property value; do
            case "$property" in
                LoadState) load_state="$value" ;;
                ActiveState) active_state="$value" ;;
                SubState) sub_state="$value" ;;
                UnitFileState) unit_file_state="$value" ;;
            esac
        done <<< "$unit_output"

        if [[ "$load_state" == "not-found" ]]; then
            continue
        fi

        if ((command_status != 0)) || [[ -z "$load_state" ]]; then
            printf '[UNKNOWN] SSH %s: не удалось получить состояние unit' "$unit"
            if ((command_status != 0)); then
                printf ' (код systemctl: %d)' "$command_status"
            fi
            if [[ -n "$unit_output" ]]; then
                printf ': %s' "$unit_output"
            fi
            printf '\n'
            ((unknown_count += 1))
            continue
        fi

        case "$unit" in
            *.service)
                unit_kind="служба"
                ((service_units_found += 1))
                ;;
            *.socket)
                unit_kind="сокет"
                ((socket_units_found += 1))
                ;;
        esac

        if [[ "$load_state" == "masked" || "$unit_file_state" == "masked" || "$unit_file_state" == "masked-runtime" ]]; then
            printf '[WARN] SSH %s: unit замаскирован (LoadState=%s, UnitFileState=%s).\n' "$unit" "${load_state:-unknown}" "${unit_file_state:-unknown}"
            ((masked_count += 1))
            continue
        fi

        if [[ "$load_state" != "loaded" ]]; then
            printf '[UNKNOWN] SSH %s: неожиданное состояние загрузки unit: %s.\n' "$unit" "$load_state"
            ((unknown_count += 1))
            continue
        fi

        if [[ "$active_state" == "active" ]]; then
            if [[ "$unit_kind" == "служба" && "$sub_state" == "running" ]]; then
                printf '[PASS] SSH %s: служба активна и выполняется.\n' "$unit"
                ((active_services += 1))
            elif [[ "$unit_kind" == "сокет" && "$sub_state" == "listening" ]]; then
                printf '[PASS] SSH %s: сокет активен и ожидает подключений (socket activation).\n' "$unit"
                ((active_sockets += 1))
            else
                printf '[WARN] SSH %s: ActiveState=active, но SubState=%s не подтверждает готовность.\n' "$unit" "${sub_state:-unknown}"
                ((unknown_count += 1))
            fi
            continue
        fi

        case "$active_state" in
            inactive)
                printf '[WARN] SSH %s: %s остановлена (SubState=%s).\n' "$unit" "$unit_kind" "${sub_state:-unknown}"
                ;;
            failed)
                printf '[WARN] SSH %s: %s завершилась с ошибкой (SubState=%s).\n' "$unit" "$unit_kind" "${sub_state:-unknown}"
                ;;
            activating|deactivating|reloading)
                printf '[WARN] SSH %s: %s находится в переходном состоянии %s (%s).\n' "$unit" "$unit_kind" "$active_state" "${sub_state:-unknown}"
                ;;
            "")
                printf '[UNKNOWN] SSH %s: systemd не вернул ActiveState.\n' "$unit"
                ((unknown_count += 1))
                ;;
            *)
                printf '[UNKNOWN] SSH %s: неизвестное ActiveState=%s (SubState=%s).\n' "$unit" "$active_state" "${sub_state:-unknown}"
                ((unknown_count += 1))
                ;;
        esac
    done

    if ((active_services > 0 || active_sockets > 0)); then
        if ((unknown_count > 0)); then
            printf '[WARN] SSH: доступный SSH unit обнаружен, но часть состояний не удалось проверить.\n'
            return 1
        fi
        if ((active_services > 0)); then
            printf '[PASS] SSH: обнаружена активная SSH-служба (%d).\n' "$active_services"
        else
            printf '[PASS] SSH: доступен активный SSH-сокет для socket activation (%d).\n' "$active_sockets"
        fi
        return 0
    fi

    if ((unknown_count > 0)); then
        printf '[WARN] SSH: состояние SSH-сервиса определить не удалось полностью.\n'
        return 1
    fi

    if ((service_units_found + socket_units_found == 0)); then
        printf '[FAIL] SSH: не обнаружены ssh.service/sshd.service или соответствующие socket units.\n'
        return 2
    fi

    if ((masked_count > 0 && service_units_found + socket_units_found == masked_count)); then
        printf '[WARN] SSH: все обнаруженные SSH units замаскированы.\n'
        return 1
    fi

    printf '[WARN] SSH: SSH units найдены, но ни служба, ни сокет не готовы принимать подключения.\n'
    return 1
}

# Do not run diagnostics when this file is sourced by a test harness.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    overall_status=0

    if require_command ip uname; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    if read_os_release; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    if check_architecture; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    if check_ethernet_link; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    if read_ip_configuration; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    if check_default_route; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    if check_dns_resolution "${DOCKSTATION_DNS_CHECK_NAME:-}"; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    if check_ssh_service; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    if print_diagnostics_report "$overall_status"; then
        check_status=0
    else
        check_status=$?
    fi
    if ((check_status > overall_status)); then
        overall_status=$check_status
    fi

    exit "$overall_status"
fi
