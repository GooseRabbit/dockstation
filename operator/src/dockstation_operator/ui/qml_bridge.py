"""Qt/QML bridge for the read-only DockStation Operator prototype."""

from __future__ import annotations

import threading
from typing import Any

from PySide6.QtCore import QObject, Property, Signal, Slot

from ..config.settings import (
    ConnectionProfile,
    load_profile,
    save_profile,
    validate_profile,
)
from ..config.station_registry import (
    StationRegistry,
    add_profile,
    load_registry,
    remove_profile,
    rename_profile,
    save_registry,
    select_profile,
)
from ..connection.connection_manager import ConnectionManager
from ..models.diagnostics_model import DiagnosticsModel


class QmlOperatorBridge(QObject):
    """Expose existing SSH/API behavior to QML without moving I/O onto the UI thread."""

    viewChanged = Signal()
    profileChanged = Signal()
    _operationFinished = Signal(bool, str, object)

    def __init__(self, connection_manager: ConnectionManager | None = None):
        super().__init__()
        self.connection_manager = connection_manager or ConnectionManager()
        self.model = DiagnosticsModel()
        self._registry = load_registry()
        legacy_profile = load_profile()
        if not self._registry.profiles and legacy_profile.host and legacy_profile.username:
            self._registry = add_profile(self._registry, legacy_profile.host, legacy_profile)
            try:
                save_registry(self._registry)
            except (OSError, ValueError):
                pass
        self._selected_profile_id = self._registry.selected_profile_id
        selected = next(
            (item for item in self._registry.profiles if item.profile_id == self._selected_profile_id),
            None,
        )
        self._profile = selected.connection if selected is not None else legacy_profile
        self._station_status: dict[str, str] = {
            item.profile_id: "UNKNOWN" for item in self._registry.profiles
        }
        self._station_alias_draft = selected.alias if selected is not None else ""
        self._ssh_port_text = "" if self._profile.ssh_port == 22 else str(self._profile.ssh_port)
        self._local_port_text = "" if self._profile.local_port == 18080 else str(self._profile.local_port)
        self._busy = False
        self._closing = False
        self._notice = "Ожидание подключения."
        self._operationFinished.connect(self._on_operation_finished)

    @Property("QVariantList", notify=viewChanged)
    def stationProfiles(self) -> list[dict[str, str | bool]]:
        items: list[dict[str, str | bool]] = []
        for item in self._registry.profiles:
            status = self._station_status.get(item.profile_id, "UNKNOWN")
            labels = {
                "CONNECTED": ("Подключено", "success"),
                "DISCONNECTED": ("Отключено", "danger"),
                "CONNECTING": ("Проверка", "warning"),
                "UNKNOWN": ("Неизвестно", "warning"),
            }
            label, color = labels.get(status, labels["UNKNOWN"])
            items.append({
                "profile_id": item.profile_id,
                "alias": item.alias,
                "status": status,
                "status_label": label,
                "status_color": color,
                "selected": item.profile_id == self._selected_profile_id,
            })
        return items

    @Property(str, notify=profileChanged)
    def selectedProfileId(self) -> str:
        return self._selected_profile_id or ""

    @Property(str, notify=profileChanged)
    def stationAlias(self) -> str:
        return self._station_alias_draft

    @Property(str, notify=profileChanged)
    def host(self) -> str:
        return self._profile.host

    @Property(str, notify=profileChanged)
    def username(self) -> str:
        return self._profile.username

    @Property(str, notify=profileChanged)
    def sshPort(self) -> str:
        return self._ssh_port_text

    @Property(str, notify=profileChanged)
    def localPort(self) -> str:
        return self._local_port_text

    @Property(str, notify=profileChanged)
    def privateKeyPath(self) -> str:
        return self._profile.private_key_path

    @Property(str, notify=profileChanged)
    def knownHostsPath(self) -> str:
        return self._profile.known_hosts_path

    @Property(bool, notify=viewChanged)
    def busy(self) -> bool:
        return self._busy

    @Property(bool, notify=viewChanged)
    def connected(self) -> bool:
        return self.connection_manager.is_connected and not self.model.stale

    @Property(str, notify=viewChanged)
    def noticeText(self) -> str:
        return self._notice

    @Property(str, notify=viewChanged)
    def connectionState(self) -> str:
        if self.connection_manager.is_connected and not self.model.stale:
            return "ПОДКЛЮЧЕНО · SSH + API"
        if self.model.connection_state == "CONNECTED":
            return "ПРОВЕРКА СОСТОЯНИЯ КАНАЛА"
        if self.model.connection_state == "DEGRADED":
            return "СВЯЗЬ НАРУШЕНА · ДАННЫЕ УСТАРЕЛИ"
        if self.model.connection_state == "DISCONNECTED":
            return "НЕ ПОДКЛЮЧЕНО"
        return "НЕТ ДОСТОВЕРНОГО СНИМКА"

    @Property(str, notify=viewChanged)
    def freshnessText(self) -> str:
        if self.model.snapshot is None:
            return "Данные ещё не получены"
        if self.model.stale:
            return "УСТАРЕВШИЕ ДАННЫЕ · " + self.model.stale_reason
        return "ДАННЫЕ АКТУАЛЬНЫ"

    @Property(str, notify=viewChanged)
    def lastUpdateText(self) -> str:
        if self.model.received_at_utc is None:
            return "Последнее обновление: —"
        local_time = self.model.received_at_utc.astimezone().strftime("%Y-%m-%d %H:%M:%S")
        return "Получено: " + local_time

    @Property(str, notify=viewChanged)
    def apiStatus(self) -> str:
        if self.model.snapshot is None:
            return "API: нет данных"
        return "API: доступен · read-only" if not self.model.stale else "API: снимок устарел"

    @Property(str, notify=viewChanged)
    def platformSummary(self) -> str:
        snapshot = self.model.snapshot
        if snapshot is None:
            return "Текущий API пока не предоставил сведения о системе."
        platform = snapshot.get("platform", {})
        if not isinstance(platform, dict):
            return "Сведения о системе недоступны."
        os_info = platform.get("os", {})
        os_name = os_info.get("name") if isinstance(os_info, dict) else None
        os_version = os_info.get("version") if isinstance(os_info, dict) else None
        kernel = platform.get("kernel")
        architecture = platform.get("architecture")
        uptime = platform.get("uptime", {})
        uptime_seconds = uptime.get("seconds") if isinstance(uptime, dict) else None
        uptime_text = self._format_uptime(uptime_seconds)
        return " · ".join(str(value) for value in (os_name, os_version, kernel, architecture, uptime_text) if value not in (None, ""))

    @Property("QVariantList", notify=viewChanged)
    def networkInterfaces(self) -> list[dict[str, str]]:
        network = self._network_section("interfaces")
        items = network.get("interfaces", []) if isinstance(network, dict) else []
        result: list[dict[str, str]] = []
        if not isinstance(items, list):
            return result
        for item in items:
            if not isinstance(item, dict):
                continue
            addresses = item.get("addresses", [])
            address_text = ", ".join(
                str(address.get("address"))
                for address in addresses
                if isinstance(address, dict) and address.get("address")
            ) if isinstance(addresses, list) else ""
            result.append({
                "name": str(item.get("name", "—")),
                "kind": str(item.get("kind", "—")),
                "state": str(item.get("oper_state") or "—"),
                "addresses": address_text or "—",
                "status": str(item.get("status", "UNKNOWN")),
            })
        return result

    @Property("QVariantList", notify=viewChanged)
    def defaultRoutes(self) -> list[dict[str, str]]:
        section = self._network_section("default_routes")
        items = section.get("routes", []) if isinstance(section, dict) else []
        result: list[dict[str, str]] = []
        if not isinstance(items, list):
            return result
        for item in items:
            if isinstance(item, dict):
                result.append({
                    "family": str(item.get("family", "—")),
                    "gateway": str(item.get("gateway") or "—"),
                    "interface": str(item.get("interface") or "—"),
                    "metric": str(item.get("metric") if item.get("metric") is not None else "—"),
                })
        return result

    @Property(str, notify=viewChanged)
    def dnsSummary(self) -> str:
        section = self._network_section("dns")
        if not isinstance(section, dict):
            return "DNS: данные не предоставлены"
        addresses = section.get("addresses", [])
        servers = ", ".join(str(value) for value in addresses) if isinstance(addresses, list) else ""
        return f"DNS: {section.get('status', 'UNKNOWN')} · {servers or 'адреса не опубликованы'}"

    @Property(str, notify=viewChanged)
    def sshSummary(self) -> str:
        section = self._network_section("ssh")
        if not isinstance(section, dict):
            return "SSH-служба: данные не предоставлены"
        return f"SSH-служба: {section.get('status', 'UNKNOWN')} · {section.get('message', '')}"

    @Property("QVariantList", notify=viewChanged)
    def events(self) -> list[str]:
        return list(reversed(self.model.client_events[-200:]))

    @Slot(str)
    def setStationAlias(self, value: str) -> None:
        self._station_alias_draft = value
        self.profileChanged.emit()

    @Slot(str)
    def addStationProfile(self, alias: str) -> None:
        if self._busy or self._closing:
            return
        try:
            self._registry = add_profile(self._registry, alias, self._profile)
            self._selected_profile_id = self._registry.selected_profile_id
            self._station_alias_draft = alias.strip()
            save_registry(self._registry)
        except (OSError, ValueError) as exc:
            self._append_event(f"Не удалось добавить локальный профиль: {exc}")
            return
        self._station_status[self._selected_profile_id] = "UNKNOWN"
        self.profileChanged.emit()
        self.viewChanged.emit()
        self._append_event("Локальный профиль подключения добавлен. Это не регистрация станции в аккаунте.")

    @Slot(str)
    def renameSelectedStation(self, alias: str) -> None:
        if self._selected_profile_id is None:
            self._append_event("Сначала добавь локальный профиль подключения.")
            return
        try:
            updated = rename_profile(self._registry, self._selected_profile_id, alias)
            save_registry(updated)
            self._registry = updated
            self._station_alias_draft = alias.strip()
        except (KeyError, OSError, ValueError) as exc:
            self._append_event(f"Не удалось переименовать профиль: {exc}")
            return
        self.profileChanged.emit()
        self.viewChanged.emit()

    @Slot(str)
    def selectStationProfile(self, profile_id: str) -> None:
        if self._busy or self._closing or profile_id == self._selected_profile_id:
            return
        try:
            updated = select_profile(self._registry, profile_id)
        except KeyError:
            self._append_event("Локальный профиль не найден.")
            return
        previous_id = self._selected_profile_id
        self.connection_manager.disconnect_from_station()
        if previous_id is not None and self._station_status.get(previous_id) == "CONNECTED":
            self._station_status[previous_id] = "DISCONNECTED"
        self._registry = updated
        self._selected_profile_id = profile_id
        selected = next(item for item in self._registry.profiles if item.profile_id == profile_id)
        self._profile = selected.connection
        self._station_alias_draft = selected.alias
        self._ssh_port_text = "" if self._profile.ssh_port == 22 else str(self._profile.ssh_port)
        self._local_port_text = "" if self._profile.local_port == 18080 else str(self._profile.local_port)
        self.model = DiagnosticsModel()
        try:
            save_registry(self._registry)
        except (OSError, ValueError) as exc:
            self._append_event(f"Не удалось сохранить выбранный профиль: {exc}")
        self.profileChanged.emit()
        self.viewChanged.emit()

    @Slot()
    def removeSelectedStation(self) -> None:
        if self._busy or self._closing or self._selected_profile_id is None:
            return
        removed_id = self._selected_profile_id
        self.connection_manager.disconnect_from_station()
        try:
            self._registry = remove_profile(self._registry, removed_id)
            save_registry(self._registry)
        except (KeyError, OSError, ValueError) as exc:
            self._append_event(f"Не удалось удалить локальный профиль: {exc}")
            return
        self._station_status.pop(removed_id, None)
        self._selected_profile_id = self._registry.selected_profile_id
        selected = next(
            (item for item in self._registry.profiles if item.profile_id == self._selected_profile_id),
            None,
        )
        self._profile = selected.connection if selected is not None else ConnectionProfile()
        self._station_alias_draft = selected.alias if selected is not None else ""
        self._ssh_port_text = "" if self._profile.ssh_port == 22 else str(self._profile.ssh_port)
        self._local_port_text = "" if self._profile.local_port == 18080 else str(self._profile.local_port)
        self.model = DiagnosticsModel()
        self.profileChanged.emit()
        self.viewChanged.emit()

    @Slot(str)
    def setHost(self, value: str) -> None:
        self._profile = self._replace_profile(host=value)
        self.profileChanged.emit()

    @Slot(str)
    def setUsername(self, value: str) -> None:
        self._profile = self._replace_profile(username=value)
        self.profileChanged.emit()

    @Slot(str)
    def setSshPort(self, value: str) -> None:
        self._ssh_port_text = value
        self.profileChanged.emit()

    @Slot(str)
    def setLocalPort(self, value: str) -> None:
        self._local_port_text = value
        self.profileChanged.emit()

    @Slot(str)
    def setPrivateKeyPath(self, value: str) -> None:
        self._profile = self._replace_profile(private_key_path=value)
        self.profileChanged.emit()

    @Slot(str)
    def setKnownHostsPath(self, value: str) -> None:
        self._profile = self._replace_profile(known_hosts_path=value)
        self.profileChanged.emit()

    @Slot()
    def saveProfile(self) -> None:
        profile = self._profile_with_port_drafts()
        errors = validate_profile(profile, require_files=False)
        if errors:
            self._append_event("Профиль не сохранён: " + "; ".join(errors))
            return
        try:
            self._profile = profile
            if self._selected_profile_id is None:
                alias = self._station_alias_draft.strip() or profile.host.strip() or "Новая станция"
                self._registry = add_profile(self._registry, alias, profile)
                self._selected_profile_id = self._registry.selected_profile_id
                self._station_alias_draft = alias
                self._station_status[self._selected_profile_id] = "UNKNOWN"
            else:
                self._registry = StationRegistry(
                    profiles=tuple(
                        type(item)(item.profile_id, item.alias, profile)
                        if item.profile_id == self._selected_profile_id else item
                        for item in self._registry.profiles
                    ),
                    selected_profile_id=self._selected_profile_id,
                )
            target = save_registry(self._registry)
        except (OSError, ValueError) as exc:
            self._append_event(f"Ошибка сохранения профиля: {exc}")
            return
        self.profileChanged.emit()
        self.viewChanged.emit()
        self._append_event(f"Локальный профиль подключения сохранён: {target}")
        self._append_event("Содержимое приватного SSH-ключа не копируется в настройки.")

    @Slot()
    def connectToStation(self) -> None:
        if self._busy or self._closing:
            return
        profile = self._profile_with_port_drafts()
        errors = validate_profile(profile, require_files=True)
        if errors:
            self._append_event("Профиль подключения не прошёл проверку: " + "; ".join(errors))
            return
        try:
            save_profile(profile)
            self._profile = profile
        except (OSError, ValueError) as exc:
            self._append_event(f"Не удалось сохранить профиль: {exc}")
            return

        profile = self._profile
        if self._selected_profile_id is not None:
            self._station_status[self._selected_profile_id] = "CONNECTING"
            self.viewChanged.emit()

        def operation() -> tuple[dict[str, Any], str]:
            result = self.connection_manager.connect_to_station(profile)
            if not result.connected:
                raise RuntimeError(result.message)
            snapshot = self.connection_manager.get_system_diagnostics()
            return snapshot, result.message

        self._start_worker(operation)

    @Slot()
    def poll(self) -> None:
        if self._busy or self._closing:
            return
        if self.connection_manager.get_connection_status() == "DEGRADED" and self.model.connection_state == "CONNECTED":
            self.connection_manager.handle_connection_loss()
            self.model.mark_diagnostics_stale("SSH-туннель неожиданно завершился.")
            self._append_event("SSH-туннель завершился; диагностические данные помечены как устаревшие.")
            return
        if self.connection_manager.is_connected:
            self.refresh()

    @Slot()
    def refresh(self) -> None:
        if self._busy or self._closing:
            return
        if not self.connection_manager.is_connected:
            self.model.mark_diagnostics_stale("Нет активного SSH/API-подключения.")
            self._append_event("Обновление пропущено: нет активного подключения.")
            return
        self._start_worker(self.connection_manager.get_system_diagnostics)

    @Slot()
    def disconnectFromStation(self) -> None:
        if self._busy or self._closing:
            return
        self._start_worker(self._disconnect_operation)

    @Slot()
    def shutdown(self) -> None:
        self._closing = True
        worker = getattr(self, "_worker_thread", None)
        if worker is not None and worker.is_alive() and worker is not threading.current_thread():
            worker.join(timeout=15.0)
        self.connection_manager.disconnect_from_station()

    def _disconnect_operation(self) -> tuple[None, str]:
        self.connection_manager.disconnect_from_station()
        return None, "Подключение закрыто пользователем."

    def _start_worker(self, operation) -> None:
        self._busy = True
        self.viewChanged.emit()

        def run() -> None:
            try:
                if self._closing:
                    return
                result = operation()
                if self._closing:
                    self.connection_manager.disconnect_from_station()
                    return
                if isinstance(result, tuple) and len(result) == 2 and isinstance(result[1], str):
                    payload, message = result
                else:
                    payload, message = result, "Диагностический снимок обновлён."
                self._operationFinished.emit(True, message, payload)
            except Exception as exc:
                self.connection_manager.handle_connection_loss()
                self._operationFinished.emit(False, str(exc), None)

        self._worker_thread = threading.Thread(target=run, name="dockstation-qml-worker", daemon=True)
        self._worker_thread.start()

    @Slot(bool, str, object)
    def _on_operation_finished(self, success: bool, message: str, payload: object) -> None:
        self._busy = False
        if self._selected_profile_id is not None:
            self._station_status[self._selected_profile_id] = (
                "CONNECTED" if success and isinstance(payload, dict)
                else "DISCONNECTED" if not success or payload is None
                else self._station_status.get(self._selected_profile_id, "UNKNOWN")
            )
        if success:
            if isinstance(payload, dict):
                self.model.apply_diagnostics(payload)
                self._append_event(message)
            else:
                self.model.clear_connection_state()
                self._append_event(message)
        else:
            self.model.mark_diagnostics_stale(message)
            self._append_event("Ошибка подключения/обмена: " + message)
        self.viewChanged.emit()

    def _append_event(self, message: str) -> None:
        self._notice = message
        self.model.append_client_event(message)
        self.viewChanged.emit()

    def _network_section(self, name: str) -> dict[str, Any] | None:
        snapshot = self.model.snapshot
        if not isinstance(snapshot, dict):
            return None
        network = snapshot.get("network")
        if not isinstance(network, dict):
            return None
        section = network.get(name)
        return section if isinstance(section, dict) else None

    def _profile_with_port_drafts(self) -> ConnectionProfile:
        ssh_port = self._parse_int(self._ssh_port_text, -1) if self._ssh_port_text.strip() else 22
        local_port = self._parse_int(self._local_port_text, -1) if self._local_port_text.strip() else 18080
        return self._replace_profile(ssh_port=ssh_port, local_port=local_port)

    def _replace_profile(self, **changes) -> ConnectionProfile:
        values = {
            "host": self._profile.host,
            "username": self._profile.username,
            "ssh_port": self._profile.ssh_port,
            "private_key_path": self._profile.private_key_path,
            "known_hosts_path": self._profile.known_hosts_path,
            "local_port": self._profile.local_port,
            "remote_api_port": self._profile.remote_api_port,
        }
        values.update(changes)
        return ConnectionProfile(**values)

    @staticmethod
    def _parse_int(value: str, fallback: int) -> int:
        try:
            return int(value.strip())
        except (AttributeError, ValueError):
            return fallback

    @staticmethod
    def _format_uptime(seconds: object) -> str:
        if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or seconds < 0:
            return "время работы недоступно"
        total = int(seconds)
        days, remainder = divmod(total, 86400)
        hours, remainder = divmod(remainder, 3600)
        minutes, _ = divmod(remainder, 60)
        return f"uptime {days} дн. {hours:02d}:{minutes:02d}"

