from __future__ import annotations

import json
import threading
from http import HTTPStatus
from types import SimpleNamespace
import urllib.error
import urllib.request

import pytest

from repka_gateway.dockstation_gateway.server import _GatewayHTTPServer, _GatewayRequestHandler, create_server


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



def test_test_adapter_route_is_disabled_by_default(running_server):
    host, port = running_server
    request = urllib.request.Request(
        f"http://{host}:{port}/api/v1/test/adapter/request",
        data=b'{"schema_version":"1.0","request_id":"test-1","operation":"PING"}',
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with pytest.raises(urllib.error.HTTPError) as error:
        urllib.request.urlopen(request, timeout=2)
    assert error.value.code == 404


@pytest.fixture
def running_test_adapter_server():
    from repka_gateway.dockstation_gateway.adapters.test_request_adapter import TestRequestAdapter
    from repka_gateway.dockstation_gateway.application.test_request_service import TestRequestService

    server = create_server(
        0,
        lambda: {"schema_version": "1.0", "summary": {"status": "PASS"}},
        TestRequestService(TestRequestAdapter()),
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def _post_json(address, body: bytes, content_type: str = "application/json"):
    host, port = address
    request = urllib.request.Request(
        f"http://{host}:{port}/api/v1/test/adapter/request",
        data=body,
        headers={"Content-Type": content_type},
        method="POST",
    )
    return urllib.request.urlopen(request, timeout=2)


def test_test_adapter_route_accepts_only_non_physical_ping(running_test_adapter_server):
    body = json.dumps({
        "schema_version": "1.0",
        "request_id": "operator-test-001",
        "operation": "PING",
    }).encode("utf-8")
    with _post_json(running_test_adapter_server, body) as response:
        assert response.status == 200
        assert response.headers.get_content_type() == "application/json"
        assert json.loads(response.read().decode("utf-8")) == {
            "schema_version": "1.0",
            "request_id": "operator-test-001",
            "status": "TEST_ADAPTER_ACCEPTED",
            "source": "TEST_ADAPTER",
            "physical_action_performed": False,
        }


def test_test_adapter_route_rejects_get(running_test_adapter_server):
    host, port = running_test_adapter_server
    with pytest.raises(urllib.error.HTTPError) as error:
        urllib.request.urlopen(
            f"http://{host}:{port}/api/v1/test/adapter/request", timeout=2
        )
    assert error.value.code == 405
    assert error.value.headers.get("Allow") == "POST"


def test_test_adapter_route_rejects_duplicate_json_keys(running_test_adapter_server):
    body = b'{"schema_version":"1.0","request_id":"test-1","request_id":"test-2","operation":"PING"}'
    with pytest.raises(urllib.error.HTTPError) as error:
        _post_json(running_test_adapter_server, body)
    assert error.value.code == 400
    assert json.loads(error.value.read())["error"]["code"] == "INVALID_REQUEST_BODY"


def test_test_adapter_route_rejects_unsupported_operation(running_test_adapter_server):
    body = json.dumps({
        "schema_version": "1.0",
        "request_id": "operator-test-002",
        "operation": "OPEN_LID",
    }).encode("utf-8")
    with pytest.raises(urllib.error.HTTPError) as error:
        _post_json(running_test_adapter_server, body)
    assert error.value.code == 400
    assert json.loads(error.value.read())["error"]["code"] == "TEST_REQUEST_INVALID"


def test_test_adapter_route_rejects_unexpected_fields(running_test_adapter_server):
    body = json.dumps({
        "schema_version": "1.0",
        "request_id": "operator-test-003",
        "operation": "PING",
        "target": "lid",
    }).encode("utf-8")
    with pytest.raises(urllib.error.HTTPError) as error:
        _post_json(running_test_adapter_server, body)
    assert error.value.code == 400


def test_test_adapter_route_rejects_non_json_content_type(running_test_adapter_server):
    with pytest.raises(urllib.error.HTTPError) as error:
        _post_json(running_test_adapter_server, b"{}", "text/plain")
    assert error.value.code == 415


def test_test_adapter_route_rejects_oversized_body(running_test_adapter_server):
    with pytest.raises(urllib.error.HTTPError) as error:
        _post_json(running_test_adapter_server, b"x" * 4097)
    assert error.value.code == 413



@pytest.mark.parametrize(
    "disconnect_error",
    [BrokenPipeError, ConnectionResetError, ConnectionAbortedError, TimeoutError],
)
def test_response_writer_treats_client_disconnect_as_normal(disconnect_error):
    class DisconnectedWriter:
        def write(self, _body):
            raise disconnect_error("client closed the response socket")

    handler = SimpleNamespace(
        command="GET",
        wfile=DisconnectedWriter(),
        close_connection=False,
        send_response=lambda _status: None,
        send_header=lambda *_args: None,
        end_headers=lambda: None,
    )

    _GatewayRequestHandler._send_json(handler, HTTPStatus.OK, {"status": "PASS"})

    assert handler.close_connection is True
