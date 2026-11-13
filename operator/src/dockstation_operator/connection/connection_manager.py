"""Coordinates the SSH tunnel and read-only API health checks."""

from __future__ import annotations

import time
from dataclasses import dataclass

from ..api.station_api_client import ApiClientError, StationApiClient
from ..config.settings import ConnectionProfile
from .ssh_tunnel import SshTunnel, SshTunnelError


# collect_diagnostics may perform 8 sequential external checks (ip x3,
# optional getent x1, systemctl x4), each bounded to 5 seconds.
# Leave a small margin so a valid slow snapshot is not abandoned by the client.
DIAGNOSTICS_API_TIMEOUT_SECONDS = 45.0


@dataclass(frozen=True)
class ConnectionResult:
    connected: bool
    message: str


class ConnectionManager:
    def __init__(self, tunnel: SshTunnel | None = None):
        self.tunnel = tunnel or SshTunnel()
        self.api_client: StationApiClient | None = None
        self.profile: ConnectionProfile | None = None
        self._connected = False

    @property
    def is_connected(self) -> bool:
        return self._connected and self.tunnel.is_running

    def connect_to_station(self, profile: ConnectionProfile, timeout_seconds: float = 12.0) -> ConnectionResult:
        self.disconnect_from_station()
        # Keep the requested profile even when the first tunnel attempt fails.
        # The caller may retry after a transient SSH/Gateway outage.
        self.profile = profile
        try:
            self.tunnel.start(profile)
        except SshTunnelError as exc:
            return ConnectionResult(False, str(exc))

        client = StationApiClient(
            f"http://127.0.0.1:{profile.local_port}",
            timeout_seconds=2.0,
            diagnostics_timeout_seconds=DIAGNOSTICS_API_TIMEOUT_SECONDS,
        )
        deadline = time.monotonic() + timeout_seconds
        last_error = "Ожидание готовности SSH-туннеля и Linux API."
        while time.monotonic() < deadline:
            if not self.tunnel.is_running:
                self.tunnel.stop()
                return ConnectionResult(False, "Процесс SSH завершился до подключения к API. Проверь адрес VM, host key и ключ пользователя.")
            try:
                client.get_health()
                self.profile = profile
                self.api_client = client
                self._connected = True
                return ConnectionResult(True, "Защищённый SSH-туннель установлен; Linux API отвечает.")
            except ApiClientError as exc:
                last_error = str(exc)
                time.sleep(0.25)
        self.tunnel.stop()
        self.api_client = None
        # Retain the selected profile so an explicit or automatic retry can
        # establish a fresh tunnel without asking the operator to re-enter it.
        self._connected = False
        return ConnectionResult(False, f"Не удалось подтвердить доступность Linux API за {timeout_seconds:g} с. {last_error}")

    def get_connection_status(self) -> str:
        if self.is_connected:
            return "CONNECTED"
        if self._connected:
            return "DEGRADED"
        return "DISCONNECTED"

    def get_tunnel_status(self) -> str:
        if self.tunnel.is_running:
            return "RUNNING"
        return "STOPPED"

    def check_connection_health(self) -> bool:
        if not self.is_connected or self.api_client is None:
            self._connected = False
            return False
        try:
            self.api_client.get_health()
            return True
        except ApiClientError:
            self.handle_connection_loss()
            return False

    def get_system_diagnostics(self) -> dict[str, object]:
        if not self.is_connected or self.api_client is None:
            raise ApiClientError("Нет активного подключения к станции.")
        return self.api_client.get_system_diagnostics()

    def handle_connection_loss(self) -> None:
        self._connected = False
        # Close the owned tunnel. Recovery policy belongs to the Operator
        # bridge; this manager never retries or replays station operations itself.
        self.tunnel.stop()

    def disconnect_from_station(self) -> None:
        self.tunnel.stop()
        self.api_client = None
        self.profile = None
        self._connected = False

    def reconnect_connection(self, timeout_seconds: float = 12.0) -> ConnectionResult:
        if self.profile is None:
            return ConnectionResult(False, "Профиль подключения не задан.")
        profile = self.profile
        self.disconnect_from_station()
        return self.connect_to_station(profile, timeout_seconds)
