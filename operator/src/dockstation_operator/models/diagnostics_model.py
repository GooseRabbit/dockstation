"""Keeps the last trusted diagnostic snapshot and marks it stale on link loss."""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any


class DiagnosticsModel:
    def __init__(self):
        self.snapshot: dict[str, Any] | None = None
        self.received_at_utc: datetime | None = None
        self.stale = True
        self.stale_reason = "Данные ещё не получены."
        self.connection_state = "DISCONNECTED"
        self.client_events: list[str] = []

    def apply_diagnostics(self, payload: dict[str, Any]) -> None:
        # The API client validates the schema before this method is called.
        self.snapshot = payload
        self.received_at_utc = datetime.now(timezone.utc)
        self.stale = False
        self.stale_reason = ""
        self.connection_state = "CONNECTED"

    def mark_diagnostics_stale(self, reason: str) -> None:
        if self.snapshot is not None:
            self.stale = True
            self.stale_reason = reason
        self.connection_state = "DEGRADED"

    def clear_connection_state(self) -> None:
        self.mark_diagnostics_stale("Подключение закрыто; сохранённые значения больше не актуальны.")
        self.connection_state = "DISCONNECTED"

    def append_client_event(self, message: str, limit: int = 500) -> None:
        timestamp = datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S")
        self.client_events.append(f"{timestamp}  {message}")
        if len(self.client_events) > limit:
            del self.client_events[: len(self.client_events) - limit]

    def diagnostics_age_seconds(self) -> float | None:
        if self.received_at_utc is None:
            return None
        return max(0.0, (datetime.now(timezone.utc) - self.received_at_utc).total_seconds())

    @staticmethod
    def format_diagnostic_value(value: object, unavailable: str = "Недоступно") -> str:
        if value is None or value == "":
            return unavailable
        if isinstance(value, bool):
            return "Да" if value else "Нет"
        return str(value)
