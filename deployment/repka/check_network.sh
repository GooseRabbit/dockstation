#!/usr/bin/env bash

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

# Do not run diagnostics when this file is sourced by a test harness.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    overall_status=0

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

    exit "$overall_status"
fi
