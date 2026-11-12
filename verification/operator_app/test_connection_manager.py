from __future__ import annotations

import threading
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
