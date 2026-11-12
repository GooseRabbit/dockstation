from __future__ import annotations

import json
from pathlib import Path
from unittest.mock import patch

from repka_gateway.dockstation_gateway import diagnostics
from repka_gateway.dockstation_gateway.diagnostics import CommandResult


def runner_for(*results: CommandResult):
    queue = list(results)
    calls: list[list[str]] = []

    def run(argv, _timeout):
        calls.append(list(argv))
        if not queue:
            raise AssertionError(f"Unexpected command: {argv}")
        return queue.pop(0)

    return run, calls


def test_read_os_release_parses_selected_keys_without_sourcing(tmp_path: Path):
    source = tmp_path / "os-release"
    source.write_text(
        'NAME="Test Linux"\nPRETTY_NAME="Test Linux 42"\nVERSION_ID="42"\nID=testlinux\n'
        'UNTRUSTED=$(touch /tmp/should-not-exist)\n',
        encoding="utf-8",
    )
    result = diagnostics.read_os_release(source)
    assert result["status"] == "PASS"
    assert result["name"] == "Test Linux 42"
    assert result["version"] == "42"
    assert result["identifier"] == "testlinux"


def test_read_os_release_missing_file_is_unavailable(tmp_path: Path):
    result = diagnostics.read_os_release(tmp_path / "missing")
    assert result["status"] == "UNAVAILABLE"
    assert result["name"] is None


def test_parse_ip_address_output_supports_ipv4_ipv6_and_peer_names():
    raw = "\n".join(
        [
            "2: enp0s3 inet 192.0.2.10/24 brd 192.0.2.255 scope global dynamic enp0s3",
            "2: enp0s3 inet6 2001:db8::10/64 scope global",
            "7: veth0@if8 inet 198.51.100.4/24 scope global veth0",
            "invalid output line",
            "3: eth0 inet not-an-ip scope global eth0",
        ]
    )
    parsed = diagnostics._parse_ip_address_output(raw)
    assert parsed == [
        {"interface": "enp0s3", "family": "IPv4", "address": "192.0.2.10/24", "scope": "global"},
        {"interface": "enp0s3", "family": "IPv6", "address": "2001:db8::10/64", "scope": "global"},
        {"interface": "veth0", "family": "IPv4", "address": "198.51.100.4/24", "scope": "global"},
    ]


def test_read_network_interfaces_maps_sysfs_and_ip_addresses(tmp_path: Path):
    iface = tmp_path / "enp0s3"
    iface.mkdir()
    (iface / "type").write_text("1\n")
    (iface / "operstate").write_text("up\n")
    (iface / "carrier").write_text("1\n")
    (iface / "device").mkdir()
    command_runner, calls = runner_for(
        CommandResult(0, "2: enp0s3 inet 192.0.2.8/24 scope global enp0s3\n", "")
    )
    with patch.object(diagnostics.shutil, "which", return_value="/usr/sbin/ip"):
        result = diagnostics.read_network_interfaces(tmp_path, command_runner=command_runner)
    assert result["status"] == "PASS"
    network_interface = result["interfaces"][0]
    assert network_interface["kind"] == "ETHERNET"
    assert network_interface["carrier"] is True
    assert network_interface["addresses"] == [
        {"family": "IPv4", "address": "192.0.2.8/24", "scope": "global"}
    ]
    assert calls == [["ip", "-o", "address", "show"]]


def test_read_network_interfaces_reports_missing_ip_tool(tmp_path: Path):
    (tmp_path / "lo").mkdir()
    with patch.object(diagnostics.shutil, "which", return_value=None):
        result = diagnostics.read_network_interfaces(tmp_path)
    assert result["status"] == "UNAVAILABLE"


def test_read_network_interfaces_warns_when_no_addresses_are_assigned(tmp_path: Path):
    iface = tmp_path / "eth0"
    iface.mkdir()
    (iface / "type").write_text("1\n")
    (iface / "operstate").write_text("up\n")
    (iface / "carrier").write_text("1\n")
    (iface / "device").mkdir()
    command_runner, _ = runner_for(CommandResult(0, "", ""))
    with patch.object(diagnostics.shutil, "which", return_value="/usr/sbin/ip"):
        result = diagnostics.read_network_interfaces(tmp_path, command_runner=command_runner)
    assert result["status"] == "WARN"
    assert result["message"] == "No valid assigned IPv4/IPv6 addresses were returned"


def test_read_network_interfaces_warns_on_malformed_address_output(tmp_path: Path):
    iface = tmp_path / "eth0"
    iface.mkdir()
    (iface / "type").write_text("1\n")
    (iface / "operstate").write_text("up\n")
    (iface / "carrier").write_text("1\n")
    (iface / "device").mkdir()
    command_runner, _ = runner_for(CommandResult(0, "not an ip address line\n", ""))
    with patch.object(diagnostics.shutil, "which", return_value="/usr/sbin/ip"):
        result = diagnostics.read_network_interfaces(tmp_path, command_runner=command_runner)
    assert result["status"] == "WARN"
    assert "No valid assigned" in result["message"]


def test_read_default_routes_parses_both_families():
    command_runner, calls = runner_for(
        CommandResult(0, json.dumps([{"dst": "default", "gateway": "192.0.2.1", "dev": "enp0s3", "metric": 100}]), ""),
        CommandResult(0, json.dumps([{"dst": "default", "gateway": "2001:db8::1", "dev": "enp0s3"}]), ""),
    )
    with patch.object(diagnostics.shutil, "which", return_value="/usr/sbin/ip"):
        result = diagnostics.read_default_routes(command_runner=command_runner)
    assert result["status"] == "PASS"
    assert len(result["routes"]) == 2
    assert result["routes"][0]["metric"] == 100
    assert calls == [
        ["ip", "-j", "-4", "route", "show", "default"],
        ["ip", "-j", "-6", "route", "show", "default"],
    ]


def test_read_default_routes_warns_when_no_default_route_exists():
    command_runner, _ = runner_for(CommandResult(0, "[]", ""), CommandResult(0, "[]", ""))
    with patch.object(diagnostics.shutil, "which", return_value="/usr/sbin/ip"):
        result = diagnostics.read_default_routes(command_runner=command_runner)
    assert result["status"] == "WARN"
    assert result["routes"] == []


def test_read_default_routes_warns_on_unexpected_json():
    command_runner, _ = runner_for(CommandResult(0, "not json", ""), CommandResult(0, "[]", ""))
    with patch.object(diagnostics.shutil, "which", return_value="/usr/sbin/ip"):
        result = diagnostics.read_default_routes(command_runner=command_runner)
    assert result["status"] == "WARN"


def test_dns_not_configured_is_not_reported_as_success():
    result = diagnostics.resolve_dns_name(None)
    assert result["status"] == "NOT_CONFIGURED"
    assert result["addresses"] == []


def test_dns_rejects_invalid_name_without_running_command():
    command_runner, calls = runner_for()
    result = diagnostics.resolve_dns_name("bad..example", command_runner)
    assert result["status"] == "FAIL"
    assert calls == []


def test_dns_resolution_deduplicates_addresses():
    command_runner, _ = runner_for(
        CommandResult(0, "192.0.2.4 STREAM example.test\n192.0.2.4 DGRAM example.test\n2001:db8::4 STREAM example.test\n", "")
    )
    result = diagnostics.resolve_dns_name("example.test", command_runner)
    assert result["status"] == "PASS"
    assert result["addresses"] == ["192.0.2.4", "2001:db8::4"]


def test_dns_timeout_is_warning():
    command_runner, _ = runner_for(CommandResult(None, error="timeout", timed_out=True))
    result = diagnostics.resolve_dns_name("example.test", command_runner)
    assert result["status"] == "WARN"


def test_ssh_socket_activation_is_accepted():
    def fake_run(argv, _timeout):
        unit = argv[-1]
        if unit == "ssh.socket":
            out = "LoadState=loaded\nActiveState=active\nSubState=listening\nUnitFileState=static\n"
        else:
            out = "LoadState=not-found\nActiveState=\nSubState=\nUnitFileState=\n"
        return CommandResult(0, out, "")

    with patch.object(diagnostics.shutil, "which", return_value="/usr/bin/systemctl"):
        result = diagnostics.read_ssh_service(fake_run)
    assert result["status"] == "PASS"
    assert result["active_units"] == ["ssh.socket"]


def test_ssh_service_running_is_accepted():
    def fake_run(argv, _timeout):
        unit = argv[-1]
        if unit == "sshd.service":
            out = "LoadState=loaded\nActiveState=active\nSubState=running\nUnitFileState=enabled\n"
        else:
            out = "LoadState=not-found\nActiveState=\nSubState=\nUnitFileState=\n"
        return CommandResult(0, out, "")

    with patch.object(diagnostics.shutil, "which", return_value="/usr/bin/systemctl"):
        result = diagnostics.read_ssh_service(fake_run)
    assert result["status"] == "PASS"
    assert result["active_units"] == ["sshd.service"]


def test_ssh_systemd_unavailable_is_warning():
    command_runner, _ = runner_for(
        CommandResult(1, "", "System has not been booted with systemd as init system (PID 1). Can't operate.")
    )
    with patch.object(diagnostics.shutil, "which", return_value="/usr/bin/systemctl"):
        result = diagnostics.read_ssh_service(command_runner)
    assert result["status"] == "WARN"


def test_ssh_no_unit_is_failure():
    def fake_run(_argv, _timeout):
        return CommandResult(0, "LoadState=not-found\nActiveState=\nSubState=\nUnitFileState=\n", "")

    with patch.object(diagnostics.shutil, "which", return_value="/usr/bin/systemctl"):
        result = diagnostics.read_ssh_service(fake_run)
    assert result["status"] == "FAIL"


def test_collect_diagnostics_returns_contract_and_explicit_scope():
    with (
        patch.object(diagnostics, "read_platform_info", return_value={
            "os": {"status": "PASS", "name": "Test Linux"},
            "kernel": "test-kernel",
            "architecture": "x86_64",
            "word_size_bits": 64,
            "uptime": {"status": "PASS", "seconds": 1.0},
        }),
        patch.object(diagnostics, "read_network_interfaces", return_value={"status": "PASS", "interfaces": []}),
        patch.object(diagnostics, "read_default_routes", return_value={"status": "WARN", "routes": []}),
        patch.object(diagnostics, "resolve_dns_name", return_value={"status": "NOT_CONFIGURED", "addresses": []}),
        patch.object(diagnostics, "read_ssh_service", return_value={"status": "PASS", "units": []}),
    ):
        result = diagnostics.collect_diagnostics()
    assert result["schema_version"] == "1.0"
    assert result["source"] == "LINUX_HOST"
    assert result["summary"]["status"] == "WARN"
    assert result["summary"]["exit_code"] == 1
    assert result["scope"]["hardware_controller_connected"] is False
    assert result["scope"]["physical_sensors_configured"] is False
    assert result["collected_at_utc"].endswith("+00:00")


def test_aggregate_status_normalizes_unconfigured_and_unavailable_to_warn():
    assert diagnostics._aggregate_status("PASS", "NOT_CONFIGURED") == "WARN"
    assert diagnostics._aggregate_status("PASS", "UNAVAILABLE") == "WARN"
    assert diagnostics._aggregate_status("PASS", "FAIL") == "FAIL"
    assert diagnostics._aggregate_status("PASS", "PASS") == "PASS"
