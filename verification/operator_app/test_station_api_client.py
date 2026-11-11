from __future__ import annotations

import copy
import json
from unittest.mock import patch

import pytest

from dockstation_operator.api.station_api_client import ApiClientError, StationApiClient


class FakeHeaders:
    def get_content_type(self):
        return "application/json"


class FakeResponse:
    def __init__(self, payload: bytes):
        self.payload = payload
        self.headers = FakeHeaders()

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self, _size=-1):
        return self.payload


def valid_diagnostics():
    return {
        "schema_version": "1.0",
        "source": "LINUX_HOST",
        "collected_at_utc": "2026-11-11T09:00:00+00:00",
        "summary": {"status": "PASS", "exit_code": 0, "message": "All checks passed"},
        "platform": {
            "os": {"status": "PASS", "message": "read", "name": "Repka OS", "version": "1", "identifier": "repka"},
            "kernel": "6.1.0",
            "architecture": "aarch64",
            "word_size_bits": 64,
            "uptime": {"status": "PASS", "message": "read", "seconds": 120.5},
        },
        "network": {
            "interfaces": {
                "status": "PASS", "message": "read",
                "interfaces": [{
                    "name": "eth0", "kind": "ETHERNET", "oper_state": "UP", "carrier": True,
                    "status": "PASS", "message": "link present",
                    "addresses": [{"family": "IPv4", "address": "192.0.2.20/24", "scope": "global"}],
                }],
            },
            "default_routes": {
                "status": "PASS", "message": "read",
                "routes": [{"family": "IPv4", "destination": "default", "gateway": "192.0.2.1", "interface": "eth0", "metric": 100, "protocol": "dhcp"}],
                "family_status": {"IPv4": "PASS", "IPv6": "PASS"},
            },
            "dns": {"status": "NOT_CONFIGURED", "message": "not configured", "hostname": None, "addresses": []},
            "ssh": {
                "status": "PASS", "message": "active", "units": [{
                    "name": "ssh.service", "load_state": "loaded", "active_state": "active",
                    "sub_state": "running", "unit_file_state": "enabled",
                }],
                "active_units": ["ssh.service"],
            },
        },
        "scope": {
            "read_only": True,
            "hardware_controller_connected": False,
            "physical_sensors_configured": False,
            "note": "Linux VM only",
        },
    }


def test_validate_health_response_requires_expected_gateway():
    StationApiClient.validate_health_response({
        "api_version": "1", "service": "dockstation-gateway", "status": "PASS", "read_only": True
    })
    with pytest.raises(ApiClientError):
        StationApiClient.validate_health_response({"api_version": "2", "service": "other", "status": "PASS", "read_only": True})


def test_validate_diagnostics_response_accepts_gateway_contract():
    StationApiClient.validate_diagnostics_response(valid_diagnostics())


def test_validate_diagnostics_response_rejects_missing_fields():
    with pytest.raises(ApiClientError, match="отсутствуют поля"):
        StationApiClient.validate_diagnostics_response({"schema_version": "1.0"})


def test_validate_diagnostics_response_rejects_wrong_nested_types():
    payload = valid_diagnostics()
    payload["platform"] = []
    with pytest.raises(ApiClientError, match="platform"):
        StationApiClient.validate_diagnostics_response(payload)


def test_validate_diagnostics_response_rejects_unknown_status():
    payload = valid_diagnostics()
    payload["network"]["dns"]["status"] = "READY"
    with pytest.raises(ApiClientError, match="статус"):
        StationApiClient.validate_diagnostics_response(payload)


def test_validate_diagnostics_response_rejects_naive_timestamp():
    payload = valid_diagnostics()
    payload["collected_at_utc"] = "2026-11-11T09:00:00"
    with pytest.raises(ApiClientError, match="часовой пояс"):
        StationApiClient.validate_diagnostics_response(payload)


def test_validate_diagnostics_response_rejects_negative_or_boolean_uptime():
    for invalid_value in (-1, True, float("nan")):
        payload = copy.deepcopy(valid_diagnostics())
        payload["platform"]["uptime"]["seconds"] = invalid_value
        with pytest.raises(ApiClientError):
            StationApiClient.validate_diagnostics_response(payload)


def test_validate_diagnostics_response_requires_read_only_scope():
    payload = valid_diagnostics()
    payload["scope"]["read_only"] = False
    with pytest.raises(ApiClientError, match="read-only"):
        StationApiClient.validate_diagnostics_response(payload)


def test_get_diagnostics_uses_get_and_validates_json():
    client = StationApiClient("http://127.0.0.1:18080")
    response = FakeResponse(json.dumps(valid_diagnostics()).encode("utf-8"))
    with patch.object(client._opener, "open", return_value=response) as open_url:
        payload = client.get_system_diagnostics()
    assert payload["source"] == "LINUX_HOST"
    request = open_url.call_args.args[0]
    assert request.method == "GET"
    assert request.full_url == "http://127.0.0.1:18080/api/v1/system/diagnostics"


def test_get_diagnostics_rejects_malformed_json():
    client = StationApiClient("http://127.0.0.1:18080")
    with patch.object(client._opener, "open", return_value=FakeResponse(b"not json")):
        with pytest.raises(ApiClientError, match="некорректный JSON"):
            client.get_system_diagnostics()


def test_get_diagnostics_rejects_oversized_response():
    client = StationApiClient("http://127.0.0.1:18080")
    with patch.object(client._opener, "open", return_value=FakeResponse(b"x" * (2 * 1024 * 1024 + 1))):
        with pytest.raises(ApiClientError, match="2 МиБ"):
            client.get_system_diagnostics()


def test_map_api_error_returns_safe_message():
    assert "HTTP" in StationApiClient.map_api_error(ApiClientError("HTTP 500: error", 500))
    assert "Не удалось" in StationApiClient.map_api_error(RuntimeError("secret"))


def test_client_refuses_non_loopback_api_url():
    with pytest.raises(ValueError, match="127.0.0.1"):
        StationApiClient("http://192.0.2.10:8080")


def test_client_refuses_https_fallback_or_other_scheme():
    with pytest.raises(ValueError):
        StationApiClient("https://127.0.0.1:8080")


def test_client_rejects_malformed_port_and_non_positive_timeout():
    with pytest.raises(ValueError):
        StationApiClient("http://127.0.0.1:invalid")
    with pytest.raises(ValueError):
        StationApiClient("http://127.0.0.1:8080", timeout_seconds=0)


def test_client_does_not_follow_redirects():
    client = StationApiClient("http://127.0.0.1:18080")
    handler = client._opener.handlers
    assert any(handler.__class__.__name__ == "_NoRedirectHandler" for handler in handler)


def test_validate_diagnostics_response_requires_utc_timestamp():
    payload = valid_diagnostics()
    payload["collected_at_utc"] = "2026-11-11T10:00:00+01:00"
    with pytest.raises(ApiClientError, match="UTC"):
        StationApiClient.validate_diagnostics_response(payload)


def test_validate_diagnostics_response_requires_consistent_summary_status_and_exit_code():
    payload = valid_diagnostics()
    payload["summary"]["status"] = "WARN"
    with pytest.raises(ApiClientError, match="не соответствует"):
        StationApiClient.validate_diagnostics_response(payload)


def test_validate_diagnostics_response_requires_route_family_statuses():
    payload = valid_diagnostics()
    payload["network"]["default_routes"]["family_status"]["IPv6"] = "READY"
    with pytest.raises(ApiClientError, match="статус"):
        StationApiClient.validate_diagnostics_response(payload)
