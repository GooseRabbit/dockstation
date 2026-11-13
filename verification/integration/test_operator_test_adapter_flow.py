"""End-to-end host test: Operator API client -> Gateway -> service -> test adapter."""

from __future__ import annotations

import threading

from dockstation_operator.api.station_api_client import StationApiClient
from repka_gateway.dockstation_gateway.adapters.test_request_adapter import TestRequestAdapter
from repka_gateway.dockstation_gateway.application.test_request_service import (
    TestRequestService as GatewayTestRequestService,
)
from repka_gateway.dockstation_gateway.server import create_server


def test_operator_gateway_application_service_and_test_adapter_flow():
    server = create_server(
        0,
        lambda: {"schema_version": "1.0", "summary": {"status": "PASS"}},
        GatewayTestRequestService(TestRequestAdapter()),
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        host, port = server.server_address
        operator_client = StationApiClient(f"http://{host}:{port}", timeout_seconds=2.0)

        receipt = operator_client.submit_test_adapter_request()

        assert receipt["schema_version"] == "1.0"
        assert receipt["request_id"]
        assert receipt["status"] == "TEST_ADAPTER_ACCEPTED"
        assert receipt["source"] == "TEST_ADAPTER"
        assert receipt["physical_action_performed"] is False

        # The original read-only health route remains available and unchanged.
        assert operator_client.get_health()["read_only"] is True
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
