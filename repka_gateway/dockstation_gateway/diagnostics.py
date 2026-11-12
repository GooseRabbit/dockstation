"""Read-only Linux platform and network diagnostics for the DockStation gateway.

The collector intentionally reports observed Linux state only. It does not
change interface configuration, routes, firewall rules, SSH units or services.
"""

from __future__ import annotations

import ipaddress
import json
import os
import platform
import re
import shutil
import socket
import struct
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Sequence


STATUS_ORDER = {"PASS": 0, "NOT_CONFIGURED": 1, "UNAVAILABLE": 1, "UNKNOWN": 1, "WARN": 1, "FAIL": 2}
COMMAND_TIMEOUT_SECONDS = 5.0


@dataclass(frozen=True)
class CommandResult:
    returncode: int | None
    stdout: str = ""
    stderr: str = ""
    error: str | None = None
    timed_out: bool = False


CommandRunner = Callable[[Sequence[str], float], CommandResult]


def run_command(argv: Sequence[str], timeout_seconds: float = COMMAND_TIMEOUT_SECONDS) -> CommandResult:
    """Run a fixed argument vector without a shell and with a hard timeout."""
    try:
        result = subprocess.run(
            list(argv),
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds,
        )
    except FileNotFoundError:
        return CommandResult(None, error=f"command not found: {argv[0]}")
    except subprocess.TimeoutExpired:
        return CommandResult(None, error=f"command timed out after {timeout_seconds:g}s", timed_out=True)
    except OSError as exc:
        return CommandResult(None, error=f"cannot execute {argv[0]}: {exc}")
    return CommandResult(result.returncode, result.stdout, result.stderr)


def _status_result(status: str, message: str, **values: object) -> dict[str, object]:
    return {"status": status, "message": message, **values}


def _read_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except (OSError, UnicodeError):
        return None


def read_os_release(path: Path = Path("/etc/os-release")) -> dict[str, object]:
    """Read selected os-release keys without sourcing or evaluating the file."""
    raw = _read_text(path)
    if raw is None:
        return _status_result("UNAVAILABLE", f"Cannot read {path}", name=None, version=None, identifier=None)

    values: dict[str, str] = {}
    for line in raw.splitlines():
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key not in {"PRETTY_NAME", "NAME", "VERSION_ID", "ID"}:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key] = value

    display_name = values.get("PRETTY_NAME") or values.get("NAME")
    if not display_name:
        return _status_result("UNKNOWN", "os-release did not contain NAME or PRETTY_NAME", name=None, version=values.get("VERSION_ID"), identifier=values.get("ID"))
    return _status_result(
        "PASS",
        "Operating system information read",
        name=display_name,
        version=values.get("VERSION_ID"),
        identifier=values.get("ID"),
    )


def read_platform_info(proc_uptime: Path = Path("/proc/uptime")) -> dict[str, object]:
    os_info = read_os_release()
    uptime_raw = _read_text(proc_uptime)
    uptime_seconds: float | None = None
    uptime_status = "PASS"
    uptime_message = "Uptime read from /proc/uptime"
    if uptime_raw:
        try:
            uptime_seconds = round(float(uptime_raw.split()[0]), 1)
            if uptime_seconds < 0:
                raise ValueError("negative uptime")
        except (ValueError, IndexError):
            uptime_status = "UNKNOWN"
            uptime_message = "Unexpected /proc/uptime format"
    else:
        uptime_status = "UNAVAILABLE"
        uptime_message = "Cannot read /proc/uptime"

    return {
        "os": os_info,
        "kernel": platform.release() or None,
        "architecture": platform.machine() or None,
        "word_size_bits": struct.calcsize("P") * 8,
        "uptime": _status_result(uptime_status, uptime_message, seconds=uptime_seconds),
    }


def _parse_ip_address_output(output: str) -> list[dict[str, str]]:
    addresses: list[dict[str, str]] = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 4:
            continue
        family_index = next((i for i, value in enumerate(fields) if value in {"inet", "inet6"}), None)
        if family_index is None or family_index + 1 >= len(fields) or len(fields) < 2:
            continue
        interface = fields[1].split("@", 1)[0]
        address = fields[family_index + 1]
        try:
            ipaddress.ip_interface(address)
        except ValueError:
            continue
        family = "IPv4" if fields[family_index] == "inet" else "IPv6"
        scope = None
        if "scope" in fields[family_index + 2 :]:
            scope_index = fields.index("scope")
            if scope_index + 1 < len(fields):
                scope = fields[scope_index + 1]
        entry = {"interface": interface, "family": family, "address": address}
        if scope:
            entry["scope"] = scope
        addresses.append(entry)
    return addresses


def _read_interface_metadata(sys_class_net: Path) -> list[dict[str, object]]:
    interfaces: list[dict[str, object]] = []
    try:
        entries = sorted(sys_class_net.iterdir(), key=lambda item: item.name)
    except OSError:
        return []

    for entry in entries:
        name = entry.name
        operstate = _read_text(entry / "operstate")
        hardware_type = _read_text(entry / "type")
        carrier_raw = _read_text(entry / "carrier")
        carrier: bool | None
        if carrier_raw == "1":
            carrier = True
        elif carrier_raw == "0":
            carrier = False
        else:
            carrier = None

        if name == "lo":
            kind = "LOOPBACK"
        elif (entry / "wireless").exists():
            kind = "WIRELESS"
        elif (entry / "bridge").exists():
            kind = "BRIDGE"
        elif hardware_type == "1" and (entry / "device").exists():
            kind = "ETHERNET"
        elif hardware_type == "1":
            kind = "VIRTUAL_OR_UNKNOWN_ETHERNET"
        else:
            kind = "OTHER"

        if kind in {"ETHERNET", "WIRELESS"} and carrier is True:
            status = "PASS"
            message = "Link carrier is present"
        elif kind in {"ETHERNET", "WIRELESS"} and carrier is False:
            status = "WARN"
            message = "Link carrier is absent"
        elif kind in {"ETHERNET", "WIRELESS"}:
            status = "UNKNOWN"
            message = "Link carrier state is unavailable"
        else:
            status = "PASS" if operstate else "UNKNOWN"
            message = "Interface metadata read" if operstate else "Interface state unavailable"

        interfaces.append(
            {
                "name": name,
                "kind": kind,
                "oper_state": operstate,
                "carrier": carrier,
                "status": status,
                "message": message,
                "addresses": [],
            }
        )
    return interfaces


def read_network_interfaces(
    sys_class_net: Path = Path("/sys/class/net"),
    ip_command: str = "ip",
    command_runner: CommandRunner = run_command,
) -> dict[str, object]:
    interfaces = _read_interface_metadata(sys_class_net)
    if not interfaces:
        return _status_result("UNAVAILABLE", f"No interfaces could be read from {sys_class_net}", interfaces=[])

    if not shutil.which(ip_command):
        return _status_result("UNAVAILABLE", f"Command not found: {ip_command}", interfaces=interfaces)

    command = command_runner([ip_command, "-o", "address", "show"], COMMAND_TIMEOUT_SECONDS)
    if command.returncode != 0:
        status = "WARN" if command.timed_out else "UNAVAILABLE"
        detail = command.error or command.stderr.strip() or f"ip command exited with {command.returncode}"
        return _status_result(status, detail, interfaces=interfaces)

    output_lines = [line for line in command.stdout.splitlines() if line.strip()]
    parsed_addresses = _parse_ip_address_output(command.stdout)
    by_name = {str(item["name"]): item for item in interfaces}
    unknown_interface_addresses = 0
    for address in parsed_addresses:
        target = by_name.get(address["interface"])
        if target is None:
            unknown_interface_addresses += 1
            continue
        target["addresses"].append({"family": address["family"], "address": address["address"], "scope": address.get("scope")})

    unparsed_lines = max(0, len(output_lines) - len(parsed_addresses))
    interface_statuses = [str(item.get("status", "UNKNOWN")) for item in interfaces]
    if not parsed_addresses:
        overall = "WARN"
        message = "No valid assigned IPv4/IPv6 addresses were returned"
    elif unparsed_lines or unknown_interface_addresses:
        overall = "WARN"
        message = (
            f"Some address output could not be matched (unparsed={unparsed_lines}, "
            f"unknown_interfaces={unknown_interface_addresses})"
        )
    elif any(status in {"WARN", "UNKNOWN", "UNAVAILABLE", "FAIL"} for status in interface_statuses):
        overall = "WARN"
        message = "Network interfaces read; one or more link states need attention"
    else:
        overall = "PASS"
        message = "Network interfaces and assigned addresses read"
    return _status_result(overall, message, interfaces=interfaces)


def read_default_routes(ip_command: str = "ip", command_runner: CommandRunner = run_command) -> dict[str, object]:
    if not shutil.which(ip_command):
        return _status_result("UNAVAILABLE", f"Command not found: {ip_command}", routes=[])

    routes: list[dict[str, object]] = []
    unknown_lines: list[str] = []
    family_results: dict[str, str] = {}
    for family_flag, family_name in (("-4", "IPv4"), ("-6", "IPv6")):
        command = command_runner([ip_command, "-j", family_flag, "route", "show", "default"], COMMAND_TIMEOUT_SECONDS)
        if command.returncode != 0:
            family_results[family_name] = "UNAVAILABLE"
            continue
        family_results[family_name] = "PASS"
        try:
            values = json.loads(command.stdout or "[]")
            if not isinstance(values, list):
                raise ValueError("route JSON is not an array")
        except (json.JSONDecodeError, ValueError):
            family_results[family_name] = "UNKNOWN"
            unknown_lines.append(f"{family_name}: invalid JSON output")
            continue
        for route in values:
            if not isinstance(route, dict) or route.get("dst", "default") != "default":
                unknown_lines.append(f"{family_name}: unexpected route entry")
                family_results[family_name] = "UNKNOWN"
                continue
            routes.append(
                {
                    "family": family_name,
                    "destination": "default",
                    "gateway": route.get("gateway"),
                    "interface": route.get("dev"),
                    "metric": route.get("metric"),
                    "protocol": route.get("protocol"),
                }
            )

    if unknown_lines:
        status = "WARN"
        message = "; ".join(unknown_lines)
    elif "UNAVAILABLE" in family_results.values() or "UNKNOWN" in family_results.values():
        status = "WARN"
        message = "Could not verify all IP route families"
    elif routes:
        status = "PASS"
        message = f"Read {len(routes)} default route(s)"
    else:
        status = "WARN"
        message = "No IPv4 or IPv6 default route is configured"
    return _status_result(status, message, routes=routes, family_status=family_results)


def resolve_dns_name(
    dns_name: str | None,
    command_runner: CommandRunner = run_command,
) -> dict[str, object]:
    if not dns_name:
        return _status_result("NOT_CONFIGURED", "Set DOCKSTATION_DNS_CHECK_NAME to enable the name-resolution check", hostname=None, addresses=[])
    if len(dns_name) > 253 or not re.fullmatch(
        r"(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(?:\.(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?))*\.?",
        dns_name,
    ):
        return _status_result("FAIL", "Invalid DNS check hostname", hostname=dns_name, addresses=[])
    command = command_runner(["getent", "ahosts", dns_name], COMMAND_TIMEOUT_SECONDS)
    if command.timed_out:
        return _status_result("WARN", "Name resolution timed out", hostname=dns_name, addresses=[])
    if command.returncode is None:
        return _status_result("UNAVAILABLE", command.error or "getent is unavailable", hostname=dns_name, addresses=[])
    if command.returncode != 0:
        detail = command.stderr.strip() or command.error or f"getent exited with {command.returncode}"
        return _status_result("WARN", detail, hostname=dns_name, addresses=[])
    addresses: list[str] = []
    for line in command.stdout.splitlines():
        candidate = line.split()[0] if line.split() else ""
        try:
            parsed = str(ipaddress.ip_address(candidate))
        except ValueError:
            continue
        if parsed not in addresses:
            addresses.append(parsed)
    if not addresses:
        return _status_result("WARN", "Resolver returned no usable addresses", hostname=dns_name, addresses=[])
    return _status_result("PASS", "Name resolved by the system name service", hostname=dns_name, addresses=addresses)


def read_ssh_service(command_runner: CommandRunner = run_command) -> dict[str, object]:
    if not shutil.which("systemctl"):
        return _status_result("UNAVAILABLE", "systemctl is not installed", units=[])

    units: list[dict[str, str]] = []
    active_services: list[str] = []
    active_sockets: list[str] = []
    unknown: list[str] = []
    masked: list[str] = []

    for unit in ("ssh.service", "sshd.service", "ssh.socket", "sshd.socket"):
        command = command_runner(
            ["systemctl", "show", "--no-pager", "--property=LoadState,ActiveState,SubState,UnitFileState", unit],
            COMMAND_TIMEOUT_SECONDS,
        )
        output = (command.stdout + "\n" + command.stderr).strip()
        if any(marker in output for marker in (
            "System has not been booted with systemd", "Failed to connect to bus", "Failed to get D-Bus connection"
        )):
            return _status_result("WARN", "systemd manager is not available", units=units)
        if command.returncode is None or command.returncode != 0:
            unknown.append(unit)
            continue
        properties: dict[str, str] = {}
        for line in command.stdout.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                properties[key] = value
        load_state = properties.get("LoadState", "")
        if load_state == "not-found":
            continue
        if not load_state:
            unknown.append(unit)
            continue
        active_state = properties.get("ActiveState", "")
        sub_state = properties.get("SubState", "")
        file_state = properties.get("UnitFileState", "")
        unit_data = {
            "name": unit,
            "load_state": load_state,
            "active_state": active_state,
            "sub_state": sub_state,
            "unit_file_state": file_state,
        }
        units.append(unit_data)
        if load_state == "masked" or file_state in {"masked", "masked-runtime"}:
            masked.append(unit)
        elif load_state != "loaded":
            unknown.append(unit)
        elif unit.endswith(".service") and active_state == "active" and sub_state == "running":
            active_services.append(unit)
        elif unit.endswith(".socket") and active_state == "active" and sub_state == "listening":
            active_sockets.append(unit)
        elif active_state not in {"inactive", "failed", "activating", "deactivating", "reloading"}:
            unknown.append(unit)

    if unknown:
        return _status_result("WARN", "Could not verify every SSH unit", units=units, unknown_units=unknown)
    if active_services:
        return _status_result("PASS", "An SSH service is active", units=units, active_units=active_services)
    if active_sockets:
        return _status_result("PASS", "An SSH socket is listening (socket activation)", units=units, active_units=active_sockets)
    if not units:
        return _status_result("FAIL", "No ssh.service/sshd.service or SSH socket unit was found", units=[])
    if len(masked) == len(units):
        return _status_result("WARN", "All discovered SSH units are masked", units=units, masked_units=masked)
    return _status_result("WARN", "SSH units exist but none is ready to accept connections", units=units)


def _aggregate_status(*statuses: str) -> str:
    if not statuses:
        return "UNKNOWN"
    if any(status == "FAIL" for status in statuses):
        return "FAIL"
    if any(status != "PASS" for status in statuses):
        return "WARN"
    return "PASS"


def collect_diagnostics(dns_name: str | None = None) -> dict[str, object]:
    """Collect one timestamped snapshot for the read-only HTTP API."""
    platform_data = read_platform_info()
    interfaces = read_network_interfaces()
    routes = read_default_routes()
    dns = resolve_dns_name(dns_name)
    ssh = read_ssh_service()
    statuses = [
        str(platform_data["os"]["status"]),  # type: ignore[index]
        str(platform_data["uptime"]["status"]),  # type: ignore[index]
        str(interfaces["status"]),
        str(routes["status"]),
        str(dns["status"]),
        str(ssh["status"]),
    ]
    summary_status = _aggregate_status(*statuses)
    exit_code = STATUS_ORDER.get(summary_status, 1)
    return {
        "schema_version": "1.0",
        "source": "LINUX_HOST",
        "collected_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "summary": {
            "status": summary_status,
            "exit_code": exit_code,
            "message": {
                "PASS": "All configured diagnostics passed",
                "WARN": "One or more diagnostics are unavailable or need attention",
                "FAIL": "A required diagnostic failed",
            }.get(summary_status, "Diagnostic result is unknown"),
        },
        "platform": platform_data,
        "network": {
            "interfaces": interfaces,
            "default_routes": routes,
            "dns": dns,
            "ssh": ssh,
        },
        "scope": {
            "read_only": True,
            "hardware_controller_connected": False,
            "physical_sensors_configured": False,
            "note": "This first VM slice reports Linux host/network state only; it does not represent ESP32 or physical station telemetry.",
        },
    }
