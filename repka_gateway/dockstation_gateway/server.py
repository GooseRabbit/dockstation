"""Loopback-only HTTP/JSON API for diagnostics and an opt-in test adapter."""

from __future__ import annotations

import json
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable
from urllib.parse import urlsplit


DiagnosticsProvider = Callable[[], dict[str, object]]
TestRequestService = Callable[[dict[str, object]], dict[str, object]]
TEST_ADAPTER_ROUTE = "/api/v1/test/adapter/request"
MAX_TEST_REQUEST_BYTES = 4096


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON object key.")
        result[key] = value
    return result


class _GatewayHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 16
    max_workers = 4

    def __init__(
        self,
        server_address: tuple[str, int],
        handler: type[BaseHTTPRequestHandler],
        provider: DiagnosticsProvider,
        test_request_service: TestRequestService | None = None,
    ):
        if server_address[0] != "127.0.0.1":
            raise ValueError("DockStation Gateway must bind only to 127.0.0.1")
        self.diagnostics_provider = provider
        self.test_request_service = test_request_service
        self._worker_slots = threading.BoundedSemaphore(self.max_workers)
        super().__init__(server_address, handler)

    def verify_request(self, request, client_address):  # type: ignore[no-untyped-def]
        return client_address[0] == "127.0.0.1"

    def process_request(self, request, client_address):  # type: ignore[no-untyped-def]
        if not self._worker_slots.acquire(blocking=False):
            try:
                request.sendall(
                    b"HTTP/1.1 503 Service Unavailable\r\n"
                    b"Connection: close\r\nContent-Length: 0\r\n\r\n"
                )
            except OSError:
                pass
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._worker_slots.release()
            raise

    def process_request_thread(self, request, client_address):  # type: ignore[no-untyped-def]
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._worker_slots.release()

    def get_request(self):  # type: ignore[no-untyped-def]
        request, client_address = super().get_request()
        request.settimeout(5.0)
        return request, client_address


class _GatewayRequestHandler(BaseHTTPRequestHandler):
    server_version = "DockStationGateway/0.1"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def log_message(self, format_string: str, *args: object) -> None:
        # Do not echo untrusted request headers or diagnostic data to stdout.
        super().log_message(format_string, *args)

    def _send_json(
        self,
        status: HTTPStatus,
        payload: dict[str, object],
        *,
        allow: str | None = None,
    ) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if self.command == "HEAD":
            body = b""
        self.send_response(status.value)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if allow is not None:
            self.send_header("Allow", allow)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Connection", "close")
        self.end_headers()
        if body:
            self.wfile.write(body)
        self.close_connection = True

    def _method_not_allowed(self, allow: str = "GET") -> None:
        self._send_json(
            HTTPStatus.METHOD_NOT_ALLOWED,
            {"error": {"code": "METHOD_NOT_ALLOWED", "message": "HTTP method is not supported for this route"}},
            allow=allow,
        )

    def do_GET(self) -> None:
        parsed = urlsplit(self.path)
        if parsed.query or parsed.fragment:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": "INVALID_REQUEST_TARGET", "message": "Query and fragment components are not supported"}})
            return
        if parsed.path == "/api/v1/health":
            self._send_json(HTTPStatus.OK, {
                "api_version": "1",
                "service": "dockstation-gateway",
                "status": "PASS",
                "read_only": True,
            })
            return
        if parsed.path == "/api/v1/system/diagnostics":
            try:
                payload = self.server.diagnostics_provider()  # type: ignore[attr-defined]
            except Exception:
                self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {
                    "error": {"code": "DIAGNOSTICS_COLLECTION_FAILED", "message": "The diagnostics snapshot could not be collected"}
                })
                return
            self._send_json(HTTPStatus.OK, payload)
            return
        if (
            parsed.path == TEST_ADAPTER_ROUTE
            and self.server.test_request_service is not None  # type: ignore[attr-defined]
        ):
            self._method_not_allowed("POST")
            return
        self._send_json(HTTPStatus.NOT_FOUND, {"error": {"code": "NOT_FOUND", "message": "API route not found"}})

    def do_POST(self) -> None:
        parsed = urlsplit(self.path)
        if parsed.query or parsed.fragment:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": "INVALID_REQUEST_TARGET", "message": "Query and fragment components are not supported"}})
            return
        if parsed.path != TEST_ADAPTER_ROUTE:
            self._method_not_allowed("GET")
            return

        service = self.server.test_request_service  # type: ignore[attr-defined]
        if service is None:
            self._send_json(HTTPStatus.NOT_FOUND, {"error": {"code": "NOT_FOUND", "message": "API route not found"}})
            return

        if self.headers.get("Transfer-Encoding") is not None:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": "UNSUPPORTED_TRANSFER_ENCODING", "message": "Chunked request bodies are not supported"}})
            return
        if self.headers.get_content_type() != "application/json":
            self._send_json(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, {"error": {"code": "UNSUPPORTED_CONTENT_TYPE", "message": "Content-Type must be application/json"}})
            return
        charset = self.headers.get_content_charset()
        if charset is not None and charset.lower() != "utf-8":
            self._send_json(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, {"error": {"code": "UNSUPPORTED_CHARSET", "message": "Request JSON must use UTF-8"}})
            return

        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            self._send_json(HTTPStatus.LENGTH_REQUIRED, {"error": {"code": "CONTENT_LENGTH_REQUIRED", "message": "Content-Length is required"}})
            return
        try:
            content_length = int(raw_length, 10)
        except ValueError:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": "INVALID_CONTENT_LENGTH", "message": "Content-Length is invalid"}})
            return
        if content_length <= 0:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": "EMPTY_REQUEST_BODY", "message": "A JSON request body is required"}})
            return
        if content_length > MAX_TEST_REQUEST_BYTES:
            self._send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": {"code": "REQUEST_TOO_LARGE", "message": "Test request exceeds the 4096-byte limit"}})
            return

        try:
            raw_body = self.rfile.read(content_length)
            if len(raw_body) != content_length:
                raise ValueError("Incomplete request body.")
            payload = json.loads(
                raw_body.decode("utf-8"),
                object_pairs_hook=_unique_json_object,
            )
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": "INVALID_REQUEST_BODY", "message": "Request body must be one valid UTF-8 JSON object without duplicate keys"}})
            return
        if type(payload) is not dict:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": "INVALID_REQUEST_BODY", "message": "Request body must be a JSON object"}})
            return

        try:
            receipt = service(payload)
        except ValueError as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": {"code": "TEST_REQUEST_INVALID", "message": str(exc)}})
            return
        except Exception:
            self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": {"code": "TEST_ADAPTER_FAILED", "message": "Test adapter could not acknowledge the request"}})
            return
        if type(receipt) is not dict:
            self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": {"code": "TEST_ADAPTER_INVALID_RECEIPT", "message": "Test adapter returned an invalid receipt"}})
            return
        self._send_json(HTTPStatus.OK, receipt)

    def do_HEAD(self) -> None:
        parsed = urlsplit(self.path)
        if parsed.path == TEST_ADAPTER_ROUTE and self.server.test_request_service is not None:  # type: ignore[attr-defined]
            self._method_not_allowed("POST")
        else:
            self._method_not_allowed("GET")

    do_PUT = _method_not_allowed
    do_PATCH = _method_not_allowed
    do_DELETE = _method_not_allowed
    do_OPTIONS = _method_not_allowed
    do_TRACE = _method_not_allowed
    do_CONNECT = _method_not_allowed

    def __getattr__(self, name: str):
        # BaseHTTPRequestHandler dispatches methods via `do_<METHOD>`.
        # Unknown extension methods are rejected consistently instead of
        # falling back to its HTML 501 response.
        if name.startswith("do_"):
            return self._method_not_allowed
        raise AttributeError(name)


def create_server(
    port: int = 8080,
    diagnostics_provider: DiagnosticsProvider | None = None,
    test_request_service: TestRequestService | None = None,
) -> _GatewayHTTPServer:
    if not isinstance(port, int) or isinstance(port, bool) or not 0 <= port <= 65535:
        raise ValueError("port must be an integer between 0 and 65535")
    if diagnostics_provider is None:
        from .diagnostics import collect_diagnostics

        diagnostics_provider = collect_diagnostics
    if test_request_service is not None and not callable(test_request_service):
        raise ValueError("test_request_service must be callable")
    return _GatewayHTTPServer(
        ("127.0.0.1", port),
        _GatewayRequestHandler,
        diagnostics_provider,
        test_request_service,
    )
