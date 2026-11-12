from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def test_gateway_cli_starts_serves_api_and_stops():
    repository_root = Path(__file__).resolve().parents[2]
    port = reserve_port()
    process = subprocess.Popen(
        [sys.executable, "-m", "repka_gateway.dockstation_gateway.main", "--port", str(port)],
        cwd=repository_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    base_url = f"http://127.0.0.1:{port}"
    try:
        deadline = time.monotonic() + 8.0
        payload = None
        last_error = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                stdout, stderr = process.communicate(timeout=1)
                raise AssertionError(f"Gateway exited early ({process.returncode})\n{stdout}\n{stderr}")
            try:
                with urllib.request.urlopen(base_url + "/api/v1/health", timeout=0.5) as response:
                    payload = json.loads(response.read().decode("utf-8"))
                break
            except (OSError, urllib.error.URLError) as exc:
                last_error = exc
                time.sleep(0.1)
        assert payload is not None, f"Gateway did not start: {last_error}"
        assert payload["service"] == "dockstation-gateway"

        with urllib.request.urlopen(base_url + "/api/v1/system/diagnostics", timeout=5) as response:
            diagnostics = json.loads(response.read().decode("utf-8"))
        assert diagnostics["schema_version"] == "1.0"
        assert diagnostics["source"] == "LINUX_HOST"
        assert diagnostics["scope"]["read_only"] is True
        assert diagnostics["scope"]["hardware_controller_connected"] is False
        assert diagnostics["summary"]["status"] in {"PASS", "WARN", "FAIL"}

        request = urllib.request.Request(base_url + "/api/v1/health", data=b"{}", method="POST")
        try:
            urllib.request.urlopen(request, timeout=2)
        except urllib.error.HTTPError as exc:
            assert exc.code == 405
        else:
            raise AssertionError("POST must be rejected")
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        if process.stdout:
            process.stdout.close()
        if process.stderr:
            process.stderr.close()
