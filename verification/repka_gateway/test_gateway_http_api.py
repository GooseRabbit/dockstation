from __future__ import annotations

import json
import threading
import urllib.error
import urllib.request

import pytest

from apps.repka_gateway.dockstation_gateway.server import _GatewayHTTPServer, _GatewayRequestHandler, create_server


@pytest.fixture
def running_server():
    server = create_server(0, lambda: {"schema_version": "1.0", "summary": {"status": "PASS"}})
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def read_json(url: str):
    with urllib.request.urlopen(url, timeout=2) as response:
        body = response.read()
        return response.status, response.headers, json.loads(body.decode("utf-8"))


def test_health_route_returns_gateway_health_only(running_server):
    host, port = running_server
    status, headers, payload = read_json(f"http://{host}:{port}/api/v1/health")
    assert status == 200
    assert headers.get_content_type() == "application/json"
    assert headers.get("Cache-Control") == "no-store"
    assert payload == {
        "api_version": "1",
        "service": "dockstation-gateway",
        "status": "PASS",
        "read_only": True,
    }


def test_diagnostics_route_returns_provider_payload(running_server):
    host, port = running_server
    status, _, payload = read_json(f"http://{host}:{port}/api/v1/system/diagnostics")
    assert status == 200
    assert payload["schema_version"] == "1.0"
    assert payload["summary"]["status"] == "PASS"


def test_unknown_route_returns_json_404(running_server):
    host, port = running_server
    with pytest.raises(urllib.error.HTTPError) as error:
        urllib.request.urlopen(f"http://{host}:{port}/unknown", timeout=2)
    assert error.value.code == 404
    assert json.loads(error.value.read())["error"]["code"] == "NOT_FOUND"


def test_non_get_method_returns_405(running_server):
    host, port = running_server
    request = urllib.request.Request(f"http://{host}:{port}/api/v1/health", data=b"{}", method="POST")
    with pytest.raises(urllib.error.HTTPError) as error:
        urllib.request.urlopen(request, timeout=2)
    assert error.value.code == 405
    assert json.loads(error.value.read())["error"]["code"] == "METHOD_NOT_ALLOWED"


def test_head_and_unknown_extension_methods_also_return_json_405(running_server):
    host, port = running_server
    for method in ("HEAD", "FROBULATE"):
        request = urllib.request.Request(f"http://{host}:{port}/api/v1/health", method=method)
        with pytest.raises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(request, timeout=2)
        assert error.value.code == 405
        body = error.value.read()
        if method == "HEAD":
            assert body == b""  # HEAD never carries a response body.
            assert error.value.headers.get("Content-Length") == "0"
        else:
            assert json.loads(body)["error"]["code"] == "METHOD_NOT_ALLOWED"


def test_query_string_is_rejected(running_server):
    host, port = running_server
    with pytest.raises(urllib.error.HTTPError) as error:
        urllib.request.urlopen(f"http://{host}:{port}/api/v1/health?debug=1", timeout=2)
    assert error.value.code == 400


def test_provider_failure_does_not_leak_exception(running_server):
    server = create_server(0, lambda: (_ for _ in ()).throw(RuntimeError("secret details")))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with pytest.raises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(f"http://{server.server_address[0]}:{server.server_address[1]}/api/v1/system/diagnostics", timeout=2)
        body = error.value.read().decode("utf-8")
        assert error.value.code == 500
        assert "secret details" not in body
        assert "DIAGNOSTICS_COLLECTION_FAILED" in body
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def test_server_refuses_non_loopback_binding():
    with pytest.raises(ValueError, match="127.0.0.1"):
        _GatewayHTTPServer(("0.0.0.0", 8080), _GatewayRequestHandler, lambda: {})


def test_server_rejects_invalid_ports():
    for port in (0 - 1, 65536, True, "8080"):
        with pytest.raises(ValueError):
            create_server(port)  # type: ignore[arg-type]
