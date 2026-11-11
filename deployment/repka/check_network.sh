#!/usr/bin/env bash

# Read-only network diagnostics for the Repka/Linux deployment.
# This first implementation checks only the physical Ethernet link.

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
            ! IFS= read -r hardware_type < "$interface_patg/type"; then
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

# Do not run diagnostics when this file is sourced by a test harness.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    check_ethernet_link
    exit $?
fi
