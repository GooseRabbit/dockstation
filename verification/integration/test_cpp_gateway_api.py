from __future__ import annotations

import json
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

from dockstation_operator.api.station_api_client import StationApiClient


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
GATEWAY_EXECUTABLE = REPOSITORY_ROOT / "build" / "dockstation_linux_bootstrap"


def _reserve_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def _get_json(url: str, timeout: float = 3.0) -> dict[str, object]:
    request = urllib.request.Request(
        url,
        headers={"Accept": "application/json"},
        method="GET",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        assert response.status == 200
        assert response.headers.get_content_type() == "application/json"
        payload = json.loads(response.read().decode("utf-8"))
    assert isinstance(payload, dict)
    return payload


def test_cpp_gateway_serves_only_loopback_read_only_api() -> None:
    if not GATEWAY_EXECUTABLE.is_file():
        pytest.skip("C++ Gateway binary is available after the Linux CMake build")

    port = _reserve_local_port()
    base_url = f"http://127.0.0.1:{port}"
    process = subprocess.Popen(
        [str(GATEWAY_EXECUTABLE), "--api-port", str(port)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        health: dict[str, object] | None = None
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                pytest.fail(f"C++ Gateway exited during startup with code {process.returncode}")
            try:
                health = _get_json(base_url + "/api/v1/health", timeout=0.5)
                break
            except (urllib.error.URLError, TimeoutError, OSError):
                time.sleep(0.1)

        assert health is not None, "C++ Gateway did not start its loopback HTTP listener"
        StationApiClient.validate_health_response(health)

        diagnostics = _get_json(base_url + "/api/v1/system/diagnostics")
        StationApiClient.validate_diagnostics_response(diagnostics)
        assert diagnostics["summary"]["status"] == "WARN"  # type: ignore[index]
        assert diagnostics["network"]["dns"]["status"] == "NOT_CONFIGURED"  # type: ignore[index]
        assert diagnostics["network"]["ssh"]["status"] == "UNAVAILABLE"  # type: ignore[index]
        assert diagnostics["scope"]["hardware_controller_connected"] is False  # type: ignore[index]
        assert diagnostics["scope"]["physical_sensors_configured"] is False  # type: ignore[index]

        with pytest.raises(urllib.error.HTTPError) as unknown_route:
            _get_json(base_url + "/api/v1/test/adapter/request")
        assert unknown_route.value.code == 404
        unknown_route.value.close()

        request = urllib.request.Request(
            base_url + "/api/v1/health",
            method="POST",
        )
        with pytest.raises(urllib.error.HTTPError) as unsupported_method:
            urllib.request.urlopen(request, timeout=3.0)
        assert unsupported_method.value.code == 405
        assert unsupported_method.value.headers.get("Allow") == "GET"
        unsupported_method.value.close()
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5.0)

    assert process.returncode == 0
