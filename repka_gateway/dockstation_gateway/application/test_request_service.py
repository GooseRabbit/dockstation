"""Application boundary for explicitly enabled, non-physical test requests."""

from __future__ import annotations

import re
from typing import Protocol


_REQUEST_ID_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,63}")
_EXPECTED_FIELDS = frozenset({"schema_version", "request_id", "operation"})


class TestRequestValidationError(ValueError):
    """Raised when a test request does not match the narrow test-only contract."""


class TestRequestAdapterPort(Protocol):
    def accept_test_request(self, request_id: str, operation: str) -> dict[str, object]:
        """Return an acknowledgement; implementations must not execute hardware."""


class TestRequestService:
    """Validate the small test contract and delegate to its configured adapter."""

    def __init__(self, adapter: TestRequestAdapterPort):
        self._adapter = adapter

    def __call__(self, payload: dict[str, object]) -> dict[str, object]:
        return self.handle_test_request(payload)

    def handle_test_request(self, payload: dict[str, object]) -> dict[str, object]:
        if type(payload) is not dict or frozenset(payload) != _EXPECTED_FIELDS:
            raise TestRequestValidationError("The test request has an invalid field set.")

        if payload.get("schema_version") != "1.0":
            raise TestRequestValidationError("Unsupported test request schema version.")

        request_id = payload.get("request_id")
        if (
            not isinstance(request_id, str)
            or _REQUEST_ID_PATTERN.fullmatch(request_id) is None
        ):
            raise TestRequestValidationError("The test request identifier is invalid.")

        operation = payload.get("operation")
        if operation != "PING":
            raise TestRequestValidationError("Only the PING test operation is supported.")

        receipt = self._adapter.accept_test_request(request_id, operation)
        if type(receipt) is not dict:
            raise RuntimeError("The test adapter returned an invalid receipt.")
        return receipt
