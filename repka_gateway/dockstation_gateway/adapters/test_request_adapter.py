"""Non-physical test adapter for the temporary DockStation VM prototype."""

from __future__ import annotations

from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class TestAdapterReceipt:
    schema_version: str
    request_id: str
    status: str
    source: str
    physical_action_performed: bool


class TestRequestAdapter:
    """Acknowledge a harmless test request without controlling hardware."""

    def accept_test_request(self, request_id: str, operation: str) -> dict[str, object]:
        if operation != "PING":
            raise ValueError("Only the PING test operation is supported.")
        receipt = TestAdapterReceipt(
            schema_version="1.0",
            request_id=request_id,
            status="TEST_ADAPTER_ACCEPTED",
            source="TEST_ADAPTER",
            physical_action_performed=False,
        )
        return asdict(receipt)
