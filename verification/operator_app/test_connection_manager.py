from __future__ import annotations

import threading
import time
from pathlib import Path

import pytest

from repka_gateway.dockstation_gateway.server import create_server
from dockstation_operator.api.station_api_client import ApiClientError
from dockstation_operator.config.settings import ConnectionProfile
from dockstation_operator.connection.connection_manager import ConnectionManager


def diagnostics_payload():
    return {
        "schema_version": "1.0",
        "source": "LINUX_HOST",
        "collected_at_utc": "2026-11-11T09:00:00+00:00",
        "summary": {"status": "PASS", "exit_code": 0, "message": "All checks passed"},
        "platform": {
            "os": {"status": "PASS", "message": "read", "name": "Repka OS", "version": "1", "identifier": "repka"},
            "kernel": "6.1.0", "architecture": "aarch64", "word_size_bits": 64,
            "uptime": {"status": "PASS", "message": "read", "seconds": 3600.0},
            "resources": {
                "memory": {
                    "status": "PASS", "message": "read",
                    "total_bytes": 1024, "available_bytes": 256,
                    "used_bytes": 768, "used_percent": 75.0,
                },
                "storage": {
                    "status": "PASS", "message": "read", "path": "/",
                    "total_bytes": 4096, "free_bytes": 1024,
                    "available_bytes": 900, "used_bytes": 3072,
                    "used_percent": 75.0,
                },
                "cpu_temperature": {
                    "status": "UNAVAILABLE", "message": "No CPU thermal zone",
                    "celsius": None, "sensor_type": None,
                },
                "cpu_load": {
                    "status": "PASS", "message": "read",
                    "one_minute": 0.1, "five_minutes": 0.2,
                    "fifteen_minutes": 0.3, "runnable_tasks": 1,
                    "total_tasks": 20,
                },
            },
        },
        "network": {
            "interfaces": {"status": "PASS", "message": "read", "interfaces": []},
            "default_routes": {"status": "WARN", "message": "No default route", "routes": [], "family_status": {"IPv4": "PASS", "IPv6": "PASS"}},
            "dns": {"status": "NOT_CONFIGURED", "message": "not configured", "hostname": None, "addresses": []},
            "ssh": {"status": "PASS", "message": "active", "units": [], "active_units": ["ssh.service"]},
        },
        "scope": {
            "read_only": True, "hardware_controller_connected": False,
            "physical_sensors_configured": False, "note": "Linux VM only",
        },
    }


class FakeTunnel:
    def __init__(self):
        self.is_running = False

    def start(self, _profile):
        self.is_running = True

    def stop(self):
        self.is_running = False


def test_connection_manager_exchanges_real_http_with_gateway_and_marks_link_loss():
    payload = diagnostics_payload()
    server = create_server(port=0, diagnostics_provider=lambda: payload)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    tunnel = FakeTunnel()
    manager = ConnectionManager(tunnel=tunnel)
    profile = ConnectionProfile(
        host="repka-vm.local", username="operator", local_port=server.server_address[1]
    )
    try:
        result = manager.connect_to_station(profile, timeout_seconds=2.0)
        assert result.connected is True
        assert manager.get_connection_status() == "CONNECTED"
        received = manager.get_system_diagnostics()
        assert received["platform"]["architecture"] == "aarch64"

        # A tunnel that disappears without an API response must no longer look connected.
        tunnel.is_running = False
        assert manager.is_connected is False
        assert manager.get_connection_status() == "DEGRADED"
        manager.handle_connection_loss()
        assert manager.get_connection_status() == "DISCONNECTED"
    finally:
        manager.disconnect_from_station()
        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)


def test_connection_manager_refuses_api_request_when_disconnected():
    manager = ConnectionManager(tunnel=FakeTunnel())
    with pytest.raises(ApiClientError, match="Нет активного подключения"):
        manager.get_system_diagnostics()


def test_operator_api_client_accepts_actual_gateway_snapshot():
    from repka_gateway.dockstation_gateway.diagnostics import collect_diagnostics
    from dockstation_operator.api.station_api_client import StationApiClient

    snapshot = collect_diagnostics()
    StationApiClient.validate_diagnostics_response(snapshot)


def test_connection_health_failure_stops_owned_tunnel():
    class FailingApiClient:
        def get_health(self):
            raise ApiClientError("Gateway unavailable")

    tunnel = FakeTunnel()
    tunnel.is_running = True
    manager = ConnectionManager(tunnel=tunnel)
    manager.api_client = FailingApiClient()
    manager._connected = True

    assert manager.check_connection_health() is False
    assert tunnel.is_running is False
    assert manager.get_connection_status() == "DISCONNECTED"



def test_failed_connection_attempt_retains_profile_for_a_later_retry():
    class InitiallyFailingTunnel(FakeTunnel):
        def __init__(self):
            super().__init__()
            self.fail_once = True

        def start(self, profile):
            if self.fail_once:
                self.fail_once = False
                from dockstation_operator.connection.ssh_tunnel import SshTunnelError
                raise SshTunnelError("temporary SSH failure")
            super().start(profile)

    server = create_server(port=0, diagnostics_provider=diagnostics_payload)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    manager = ConnectionManager(tunnel=InitiallyFailingTunnel())
    profile = ConnectionProfile(
        host="repka-vm.local", username="operator",
        local_port=server.server_address[1],
    )
    try:
        first = manager.connect_to_station(profile, timeout_seconds=0.1)
        assert first.connected is False
        assert manager.profile == profile

        # The retry reuses the exact same profile and establishes a fresh tunnel.
        result = manager.connect_to_station(manager.profile, timeout_seconds=2.0)
        assert result.connected is True
        assert manager.profile == profile
    finally:
        manager.disconnect_from_station()
        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)



def test_connection_manager_recovers_after_gateway_restart_and_reads_fresh_snapshot():
    payload = diagnostics_payload()
    payload["test_marker"] = "before-restart"
    server = create_server(port=0, diagnostics_provider=lambda: payload)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    tunnel = FakeTunnel()
    manager = ConnectionManager(tunnel=tunnel)
    profile = ConnectionProfile(
        host="repka-vm.local", username="operator", local_port=port
    )
    restarted_server = None
    restarted_thread = None
    try:
        connected = manager.connect_to_station(profile, timeout_seconds=2.0)
        assert connected.connected is True
        before = manager.get_system_diagnostics()
        assert before["test_marker"] == "before-restart"

        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)
        manager.handle_connection_loss()
        assert manager.profile == profile
        assert manager.is_connected is False

        payload["test_marker"] = "after-restart"
        restarted_server = create_server(port=port, diagnostics_provider=lambda: payload)
        restarted_thread = threading.Thread(
            target=restarted_server.serve_forever, daemon=True
        )
        restarted_thread.start()

        recovered = manager.reconnect_connection(timeout_seconds=2.0)
        assert recovered.connected is True
        after = manager.get_system_diagnostics()
        assert after["test_marker"] == "after-restart"
        assert manager.get_connection_status() == "CONNECTED"
    finally:
        manager.disconnect_from_station()
        if restarted_server is not None:
            restarted_server.shutdown()
            restarted_server.server_close()
        if restarted_thread is not None:
            restarted_thread.join(timeout=2.0)



def test_connection_manager_allows_slow_diagnostics_beyond_health_timeout():
    payload = diagnostics_payload()

    def slow_diagnostics():
        time.sleep(2.2)
        return payload

    server = create_server(port=0, diagnostics_provider=slow_diagnostics)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    manager = ConnectionManager(tunnel=FakeTunnel())
    profile = ConnectionProfile(
        host="repka-vm.local",
        username="operator",
        local_port=server.server_address[1],
    )
    try:
        connected = manager.connect_to_station(profile, timeout_seconds=2.0)
        assert connected.connected is True
        received = manager.get_system_diagnostics()
        assert received["source"] == "LINUX_HOST"
    finally:
        manager.disconnect_from_station()
        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)
