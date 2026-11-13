"""Qt/QML bridge for the read-only DockStation Operator prototype."""

from __future__ import annotations

import threading
import time
from pathlib import Path
from typing import Any

from PySide6.QtCore import QObject, Property, Signal, Slot
from PySide6.QtGui import QGuiApplication

from ..config.settings import (
    ConnectionProfile,
    load_profile,
    validate_profile,
)
from ..config.station_registry import (
    StationProfile,
    StationRegistry,
    add_profile,
    allocate_local_port,
    repair_duplicate_local_ports,
    move_profile,
    load_registry,
    remove_profile,
    rename_profile,
    save_registry,
    select_profile,
    station_registry_path,
)
from ..connection.connection_manager import ConnectionManager
from ..models.diagnostics_model import DiagnosticsModel


class QmlOperatorBridge(QObject):
    """Expose existing SSH/API behavior to QML without moving I/O onto the UI thread."""

    viewChanged = Signal()
    profileChanged = Signal()
    _operationFinished = Signal(str, bool, str, object, bool, bool)

    def __init__(
        self,
        connection_manager: ConnectionManager | None = None,
        connection_manager_factory=None,
    ):
        super().__init__()
        self._connection_manager_factory = connection_manager_factory or ConnectionManager
        self._initial_connection_manager = connection_manager or ConnectionManager()
        self._connection_managers: dict[str, ConnectionManager] = {}
        self._default_model = DiagnosticsModel()
        self._models: dict[str, DiagnosticsModel] = {}
        registry_path = station_registry_path()
        loaded_registry = load_registry(registry_path)
        self._registry = repair_duplicate_local_ports(loaded_registry)
        if self._registry != loaded_registry:
            try:
                save_registry(self._registry, registry_path)
            except (OSError, ValueError):
                pass
        legacy_profile = load_profile()
        if not registry_path.exists() and legacy_profile.host and legacy_profile.username:
            try:
                migrated_registry = add_profile(self._registry, legacy_profile.host, legacy_profile)
                save_registry(migrated_registry, registry_path)
                self._registry = migrated_registry
            except (OSError, ValueError):
                pass

        self._selected_profile_id = self._registry.selected_profile_id
        for item in self._registry.profiles:
            self._models[item.profile_id] = DiagnosticsModel()
        self._station_status: dict[str, str] = {
            item.profile_id: "UNKNOWN" for item in self._registry.profiles
        }
        self._station_alias_draft = ""
        self._profile = legacy_profile
        self._ssh_port_text = "" if self._profile.ssh_port == 22 else str(self._profile.ssh_port)
        self._local_port_text = "" if self._profile.local_port == 18080 else str(self._profile.local_port)

        self._busy_profiles: set[str] = set()
        self._worker_threads: dict[str, threading.Thread] = {}
        self._reconnect_pending_by_profile: dict[str, bool] = {}
        self._reconnect_attempt_by_profile: dict[str, int] = {}
        self._next_reconnect_by_profile: dict[str, float] = {}
        self._station_profiles_expanded = False
        self._busy = False
        self._closing = False
        self._reconnect_pending = False
        self._reconnect_attempt = 0
        self._next_reconnect_at = 0.0
        self._notice = "Ожидание подключения."
        self._logs_action_status = ""
        self.connection_manager = (
            self._manager_for(self._selected_profile_id)
            if self._selected_profile_id is not None
            else self._initial_connection_manager
        )
        self.model = self._model_for(self._selected_profile_id)
        selected = next(
            (item for item in self._registry.profiles if item.profile_id == self._selected_profile_id),
            None,
        )
        if selected is not None:
            self._profile = selected.connection
            self._station_alias_draft = selected.alias
            self._ssh_port_text = "" if self._profile.ssh_port == 22 else str(self._profile.ssh_port)
            self._local_port_text = "" if self._profile.local_port == 18080 else str(self._profile.local_port)
        self._sync_active_session_flags()
        self._operationFinished.connect(self._on_operation_finished)

    def _session_key(self, profile_id: str | None) -> str:
        return profile_id or "__default__"

    def _manager_for(self, profile_id: str | None) -> ConnectionManager:
        key = self._session_key(profile_id)
        manager = self._connection_managers.get(key)
        if manager is None:
            if self._initial_connection_manager is not None:
                manager = self._initial_connection_manager
                self._initial_connection_manager = None
            else:
                manager = self._connection_manager_factory()
            self._connection_managers[key] = manager
        return manager

    def _model_for(self, profile_id: str | None) -> DiagnosticsModel:
        if profile_id is None:
            return self._default_model
        return self._models.setdefault(profile_id, DiagnosticsModel())

    def _sync_active_session_flags(self) -> None:
        key = self._session_key(self._selected_profile_id)
        self._busy = key in self._busy_profiles
        self._reconnect_pending = self._reconnect_pending_by_profile.get(key, False)
        self._reconnect_attempt = self._reconnect_attempt_by_profile.get(key, 0)
        self._next_reconnect_at = self._next_reconnect_by_profile.get(key, 0.0)

    @Property("QVariantList", notify=viewChanged)
    def stationProfiles(self) -> list[dict[str, str | bool]]:
        items: list[dict[str, str | bool]] = []
        profiles = self._registry.profiles
        for index, item in enumerate(profiles):
            status = self._station_status.get(item.profile_id, "UNKNOWN")
            manager = self._connection_managers.get(self._session_key(item.profile_id))
            model = self._models.get(item.profile_id)
            if manager is not None and manager.is_connected and model is not None and not model.stale:
                status = "CONNECTED"
                self._station_status[item.profile_id] = status
            labels = {
                "CONNECTED": ("Подключено", "success"),
                "DISCONNECTED": ("Отключено", "danger"),
                "CONNECTING": ("Подключение", "warning"),
                "UNKNOWN": ("Ошибка / неизвестно", "warning"),
            }
            label, color = labels.get(status, labels["UNKNOWN"])
            items.append({
                "profile_id": item.profile_id,
                "alias": item.alias,
                "status": status,
                "status_label": label,
                "status_color": color,
                "selected": item.profile_id == self._selected_profile_id,
                "can_move_up": index > 0,
                "can_move_down": index < len(profiles) - 1,
            })
        return items

    @Property("QVariantList", notify=viewChanged)
    def visibleStationProfiles(self) -> list[dict[str, str | bool]]:
        profiles = self.stationProfiles
        return profiles if self._station_profiles_expanded else profiles[:3]

    @Property(bool, notify=viewChanged)
    def hasMoreStationProfiles(self) -> bool:
        return len(self._registry.profiles) > 3

    @Property(bool, notify=viewChanged)
    def stationProfilesExpanded(self) -> bool:
        return self._station_profiles_expanded

    @Slot()
    def toggleStationProfilesExpanded(self) -> None:
        if self.hasMoreStationProfiles:
            self._station_profiles_expanded = not self._station_profiles_expanded
            self.viewChanged.emit()

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
    def reconnecting(self) -> bool:
        return self._reconnect_pending_by_profile.get(
            self._session_key(self._selected_profile_id), self._reconnect_pending
        )

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

    @Property("QVariantMap", notify=viewChanged)
    def systemMetrics(self) -> dict[str, str]:
        snapshot = self.model.snapshot
        platform = snapshot.get("platform", {}) if isinstance(snapshot, dict) else {}
        resources = platform.get("resources", {}) if isinstance(platform, dict) else {}
        if not isinstance(resources, dict):
            resources = {}

        def section(name: str) -> dict[str, Any]:
            value = resources.get(name, {})
            return value if isinstance(value, dict) else {}

        def format_bytes(value: object) -> str:
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
                return "—"
            amount = float(value)
            for unit in ("Б", "КиБ", "МиБ", "ГиБ", "ТиБ"):
                if amount < 1024.0 or unit == "ТиБ":
                    return f"{amount:.1f} {unit}" if unit != "Б" else f"{int(amount)} {unit}"
                amount /= 1024.0
            return "—"

        def metric_status(item: dict[str, Any]) -> str:
            if self.model.snapshot is None:
                return "Нет диагностического снимка"
            if self.model.stale:
                return "Значение из устаревшего снимка"
            return str(item.get("message") or "Источник не предоставил значение")

        temperature = section("cpu_temperature")
        temperature_ok = temperature.get("status") == "PASS" and isinstance(temperature.get("celsius"), (int, float))
        memory = section("memory")
        memory_ok = memory.get("status") == "PASS" and isinstance(memory.get("used_percent"), (int, float))
        storage = section("storage")
        storage_ok = storage.get("status") == "PASS" and isinstance(storage.get("used_percent"), (int, float))
        load = section("cpu_load")
        load_ok = load.get("status") == "PASS" and all(
            isinstance(load.get(key), (int, float))
            for key in ("one_minute", "five_minutes", "fifteen_minutes")
        )
        return {
            "cpu_temperature_value": f"{temperature['celsius']:.1f} °C" if temperature_ok else "Недоступно",
            "cpu_temperature_detail": str(temperature.get("sensor_type") or metric_status(temperature)),
            "cpu_temperature_color": "#46A982" if temperature_ok and not self.model.stale else "#D6A34A",
            "memory_value": f"{memory['used_percent']:.1f} %" if memory_ok else "Недоступно",
            "memory_detail": (
                f"Занято {format_bytes(memory.get('used_bytes'))} из {format_bytes(memory.get('total_bytes'))}; "
                f"доступно {format_bytes(memory.get('available_bytes'))}"
                if memory_ok else metric_status(memory)
            ),
            "memory_color": "#46A982" if memory_ok and not self.model.stale else "#D6A34A",
            "storage_value": f"{storage['used_percent']:.1f} %" if storage_ok else "Недоступно",
            "storage_detail": (
                f"Занято {format_bytes(storage.get('used_bytes'))} из {format_bytes(storage.get('total_bytes'))}; "
                f"доступно {format_bytes(storage.get('available_bytes'))}"
                if storage_ok else metric_status(storage)
            ),
            "storage_color": "#46A982" if storage_ok and not self.model.stale else "#D6A34A",
            "load_value": (
                f"{load['one_minute']:.2f} / {load['five_minutes']:.2f} / {load['fifteen_minutes']:.2f}"
                if load_ok else "Недоступно"
            ),
            "load_detail": (
                f"Задач: {load.get('runnable_tasks', '—')} выполняется/ожидает из {load.get('total_tasks', '—')}"
                if load_ok else metric_status(load)
            ),
            "load_color": "#46A982" if load_ok and not self.model.stale else "#D6A34A",
        }

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
        """Return the selected station's Operator events, newest first."""
        return list(reversed(self.model.client_events))

    @Property(str, notify=viewChanged)
    def eventsText(self) -> str:
        """Return only the selected station's retained Operator events."""
        return "\n".join(self.events)

    @Property(str, notify=viewChanged)
    def logsActionStatus(self) -> str:
        return self._logs_action_status

    @Slot(result=bool)
    def copyLogs(self) -> bool:
        """Copy all currently retained Operator events to the desktop clipboard."""
        try:
            clipboard = QGuiApplication.clipboard()
            if clipboard is None:
                raise RuntimeError("Системный буфер обмена недоступен.")
            clipboard.setText(self.eventsText)
        except (AttributeError, RuntimeError) as exc:
            self._logs_action_status = f"Не удалось скопировать логи: {exc}"
            self.viewChanged.emit()
            return False
        count = len(self.events)
        self._logs_action_status = (
            f"Скопировано записей: {count}."
            if count else "Журнал пуст; буфер обмена очищен."
        )
        self.viewChanged.emit()
        return True

    @Slot(str, result=bool)
    def saveLogs(self, file_path: str) -> bool:
        """Save the complete retained Operator event buffer as UTF-8 plain text."""
        if not isinstance(file_path, str) or not file_path.strip():
            self._logs_action_status = "Не выбран путь для сохранения журнала."
            self.viewChanged.emit()
            return False
        target = Path(file_path)
        contents = self.eventsText
        try:
            target.write_text(
                contents + ("\n" if contents else ""),
                encoding="utf-8",
                newline="\n",
            )
        except (OSError, ValueError) as exc:
            self._logs_action_status = f"Не удалось сохранить логи: {exc}"
            self.viewChanged.emit()
            return False
        count = len(self.events)
        self._logs_action_status = (
            f"Сохранено записей: {count} · {target.name}"
            if count else f"Сохранён пустой журнал · {target.name}"
        )
        self.viewChanged.emit()
        return True

    @Slot(str)
    def setStationAlias(self, value: str) -> None:
        self._station_alias_draft = value
        self.profileChanged.emit()

    @Slot(str)
    def addStationProfile(self, alias: str) -> None:
        if self._closing:
            return
        try:
            local_port = allocate_local_port(self._registry)
            new_profile = ConnectionProfile(local_port=local_port)
            updated_registry = add_profile(self._registry, alias, new_profile)
            save_registry(updated_registry)
        except (OSError, ValueError) as exc:
            self._append_event(f"Не удалось добавить локальный профиль: {exc}")
            return
        self._registry = updated_registry
        self._selected_profile_id = self._registry.selected_profile_id
        self._models[self._selected_profile_id] = DiagnosticsModel()
        self._station_status[self._selected_profile_id] = "UNKNOWN"
        self.connection_manager = self._manager_for(self._selected_profile_id)
        self.model = self._model_for(self._selected_profile_id)
        self._profile = new_profile
        self._station_alias_draft = alias.strip()
        self._ssh_port_text = ""
        self._local_port_text = str(local_port)
        self._sync_active_session_flags()
        self.profileChanged.emit()
        self.viewChanged.emit()
        self._append_event("Локальный профиль добавлен. Ранее подключённые станции продолжают работать.")

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
        if self._closing or profile_id == self._selected_profile_id:
            return
        try:
            updated = select_profile(self._registry, profile_id)
        except KeyError:
            self._append_event("Локальный профиль не найден.")
            return
        self._registry = updated
        self._selected_profile_id = profile_id
        selected = next(item for item in self._registry.profiles if item.profile_id == profile_id)
        self.connection_manager = self._manager_for(profile_id)
        self.model = self._model_for(profile_id)
        self._profile = selected.connection
        self._station_alias_draft = selected.alias
        self._ssh_port_text = "" if self._profile.ssh_port == 22 else str(self._profile.ssh_port)
        self._local_port_text = "" if self._profile.local_port == 18080 else str(self._profile.local_port)
        self._sync_active_session_flags()
        try:
            save_registry(self._registry)
        except (OSError, ValueError) as exc:
            self._append_event(f"Не удалось сохранить выбранный профиль: {exc}", profile_id)
        self.profileChanged.emit()
        self.viewChanged.emit()

    @Slot(str, int)
    def moveStationProfile(self, profile_id: str, offset: int) -> None:
        try:
            updated = move_profile(self._registry, profile_id, offset)
            if updated == self._registry:
                return
            save_registry(updated)
            self._registry = updated
        except (KeyError, OSError, ValueError) as exc:
            self._append_event(f"Не удалось изменить порядок станций: {exc}")
            return
        self.viewChanged.emit()

    @Slot("QVariantList", result=bool)
    def reorderStationProfiles(self, profile_ids: list[str]) -> bool:
        """Persist a drag-reordered visible profile sequence without changing sessions."""
        if self._closing or not isinstance(profile_ids, (list, tuple)):
            return False

        requested_ids = list(profile_ids)
        if any(not isinstance(profile_id, str) or not profile_id for profile_id in requested_ids):
            return False

        current_profiles = self._registry.profiles
        visible_profiles = (
            current_profiles
            if self._station_profiles_expanded
            else current_profiles[:3]
        )
        expected_visible_ids = [item.profile_id for item in visible_profiles]
        if (
            len(requested_ids) != len(expected_visible_ids)
            or len(set(requested_ids)) != len(requested_ids)
            or set(requested_ids) != set(expected_visible_ids)
        ):
            self._append_event("Изменение порядка станций отменено: список профилей изменился.")
            return False

        hidden_ids = (
            []
            if self._station_profiles_expanded
            else [item.profile_id for item in current_profiles[3:]]
        )
        ordered_ids = requested_ids + hidden_ids
        profiles_by_id = {item.profile_id: item for item in current_profiles}
        updated = StationRegistry(
            profiles=tuple(profiles_by_id[profile_id] for profile_id in ordered_ids),
            selected_profile_id=self._registry.selected_profile_id,
        )
        if updated == self._registry:
            return True

        try:
            save_registry(updated)
        except (OSError, ValueError) as exc:
            self._append_event(f"Не удалось сохранить новый порядок станций: {exc}")
            return False

        self._registry = updated
        self.viewChanged.emit()
        return True

    @Slot()
    def removeSelectedStation(self) -> None:
        if self._closing or self._selected_profile_id is None:
            return
        removed_id = self._selected_profile_id
        key = self._session_key(removed_id)
        if key in self._busy_profiles:
            self._append_event("Нельзя удалить станцию, пока выполняется операция обмена.")
            return
        manager = self._connection_managers.get(key)
        if manager is not None:
            manager.disconnect_from_station()
        try:
            updated = remove_profile(self._registry, removed_id)
            save_registry(updated)
        except (KeyError, OSError, ValueError) as exc:
            self._append_event(f"Не удалось удалить локальный профиль: {exc}")
            return
        self._registry = updated
        self._station_status.pop(removed_id, None)
        self._models.pop(removed_id, None)
        self._connection_managers.pop(key, None)
        self._reconnect_pending_by_profile.pop(key, None)
        self._reconnect_attempt_by_profile.pop(key, None)
        self._next_reconnect_by_profile.pop(key, None)
        self._selected_profile_id = self._registry.selected_profile_id
        selected = next((item for item in self._registry.profiles if item.profile_id == self._selected_profile_id), None)
        self.connection_manager = self._manager_for(self._selected_profile_id)
        self.model = self._model_for(self._selected_profile_id)
        self._profile = selected.connection if selected is not None else ConnectionProfile()
        self._station_alias_draft = selected.alias if selected is not None else ""
        self._ssh_port_text = "" if self._profile.ssh_port == 22 else str(self._profile.ssh_port)
        self._local_port_text = "" if self._profile.local_port == 18080 else str(self._profile.local_port)
        self._sync_active_session_flags()
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

    @Slot(result=bool)
    def saveProfile(self) -> bool:
        profile = self._profile_with_port_drafts()
        errors = validate_profile(profile, require_files=False)
        if errors:
            self._append_event("Профиль не сохранён: " + "; ".join(errors))
            return False
        try:
            self._profile = profile
            if self._selected_profile_id is None:
                # A profile may be created from the legacy single-profile editor.
                # Carry its retry state to the newly assigned profile key so the
                # first reconnect remains attached to the same SSH/API session.
                previous_key = self._session_key(None)
                pending = self._reconnect_pending_by_profile.pop(
                    previous_key, self._reconnect_pending
                )
                attempts = self._reconnect_attempt_by_profile.pop(
                    previous_key, self._reconnect_attempt
                )
                next_attempt = self._next_reconnect_by_profile.pop(
                    previous_key, self._next_reconnect_at
                )
                alias = self._station_alias_draft.strip() or profile.host.strip() or "Новая станция"
                self._registry = add_profile(self._registry, alias, profile)
                self._selected_profile_id = self._registry.selected_profile_id
                new_key = self._session_key(self._selected_profile_id)
                if pending:
                    self._reconnect_pending_by_profile[new_key] = True
                    self._reconnect_attempt_by_profile[new_key] = attempts
                    self._next_reconnect_by_profile[new_key] = next_attempt
                self._models[self._selected_profile_id] = DiagnosticsModel()
                self._station_status[self._selected_profile_id] = "UNKNOWN"
                self.connection_manager = self._manager_for(self._selected_profile_id)
                self.model = self._model_for(self._selected_profile_id)
                self._station_alias_draft = alias
            else:
                selected_item = next(item for item in self._registry.profiles if item.profile_id == self._selected_profile_id)
                if self._station_alias_draft.strip() != selected_item.alias:
                    self._registry = rename_profile(self._registry, self._selected_profile_id, self._station_alias_draft)
                self._registry = StationRegistry(
                    profiles=tuple(
                        StationProfile(item.profile_id, item.alias, profile)
                        if item.profile_id == self._selected_profile_id else item
                        for item in self._registry.profiles
                    ),
                    selected_profile_id=self._selected_profile_id,
                )
            target = save_registry(self._registry)
        except (OSError, ValueError) as exc:
            self._append_event(f"Ошибка сохранения профиля: {exc}")
            return False
        manager = self._manager_for(self._selected_profile_id)
        if not manager.is_connected:
            manager.profile = profile
        self._profile = profile
        self._sync_active_session_flags()
        self.profileChanged.emit()
        self.viewChanged.emit()
        self._append_event(f"Локальный профиль подключения сохранён: {target}")
        self._append_event("Содержимое приватного SSH-ключа не копируется в настройки.")
        return True

    @Slot()
    def connectToStation(self) -> None:
        profile_id = self._selected_profile_id
        key = self._session_key(profile_id)
        if self._closing or key in self._busy_profiles:
            return
        profile = self._profile_with_port_drafts()
        errors = validate_profile(profile, require_files=True)
        if errors:
            self._append_event("Профиль подключения не прошёл проверку: " + "; ".join(errors))
            return
        if not self.saveProfile():
            return
        profile_id = self._selected_profile_id
        key = self._session_key(profile_id)
        profile = self._profile
        manager = self._manager_for(profile_id)
        if profile_id is not None:
            self._station_status[profile_id] = "CONNECTING"
        self.viewChanged.emit()

        def operation() -> tuple[dict[str, Any], str]:
            result = manager.connect_to_station(profile)
            if not result.connected:
                raise RuntimeError(result.message)
            return manager.get_system_diagnostics(), result.message

        self._start_worker(profile_id, operation)

    @Slot()
    def poll(self) -> None:
        if self._closing:
            return
        active_key = self._session_key(self._selected_profile_id)
        if self._reconnect_pending:
            self._reconnect_pending_by_profile.setdefault(active_key, True)
            self._reconnect_attempt_by_profile.setdefault(active_key, self._reconnect_attempt)
            self._next_reconnect_by_profile.setdefault(active_key, self._next_reconnect_at)
        profile_ids = [item.profile_id for item in self._registry.profiles]
        if not profile_ids and (
            self._connection_managers.get("__default__") is not None
            or self._initial_connection_manager is not None
        ):
            profile_ids = [None]
        now = time.monotonic()
        for profile_id in profile_ids:
            key = self._session_key(profile_id)
            if key in self._busy_profiles:
                continue
            manager = self._connection_managers.get(key)
            if manager is None:
                continue
            if self._reconnect_pending_by_profile.get(key, False):
                if now >= self._next_reconnect_by_profile.get(key, 0.0):
                    self._start_worker(profile_id, lambda manager=manager: self._reconnect_operation(manager), is_reconnect=True)
                continue
            status_getter = getattr(manager, "get_connection_status", None)
            connection_status = status_getter() if callable(status_getter) else ("CONNECTED" if manager.is_connected else "DISCONNECTED")
            model = self._model_for(profile_id)
            if connection_status == "DEGRADED" and model.connection_state in {"CONNECTED", "DEGRADED"}:
                self._schedule_reconnect("SSH-туннель неожиданно завершился.", profile_id)
            elif manager.is_connected:
                self._start_worker(profile_id, manager.get_system_diagnostics)

    @Slot()
    def refresh(self) -> None:
        profile_id = self._selected_profile_id
        key = self._session_key(profile_id)
        if self._closing or key in self._busy_profiles:
            return
        manager = self._manager_for(profile_id)
        model = self._model_for(profile_id)
        if not manager.is_connected:
            if profile_id is not None:
                self._station_status[profile_id] = "UNKNOWN"
            model.mark_diagnostics_stale("Нет активного SSH/API-подключения.")
            self._append_event("Обновление пропущено: нет активного подключения.", profile_id)
            return
        self._start_worker(profile_id, manager.get_system_diagnostics)

    @Slot()
    def disconnectFromStation(self) -> None:
        profile_id = self._selected_profile_id
        key = self._session_key(profile_id)
        if self._closing or key in self._busy_profiles:
            return
        self._reconnect_pending_by_profile[key] = False
        self._reconnect_attempt_by_profile[key] = 0
        manager = self._manager_for(profile_id)

        def operation() -> tuple[None, str]:
            manager.disconnect_from_station()
            return None, "Подключение закрыто пользователем."

        self._start_worker(profile_id, operation)
        self._sync_active_session_flags()

    @Slot()
    def shutdown(self) -> None:
        self._closing = True
        deadline = time.monotonic() + 15.0
        for worker in list(self._worker_threads.values()):
            if worker.is_alive() and worker is not threading.current_thread():
                worker.join(timeout=max(0.0, deadline - time.monotonic()))
        managers = list(self._connection_managers.values())
        if self._initial_connection_manager is not None:
            managers.append(self._initial_connection_manager)
        for manager in managers:
            try:
                manager.disconnect_from_station()
            except Exception:
                pass

    def _reconnect_operation(self, manager: ConnectionManager | None = None) -> tuple[dict[str, Any], str]:
        selected_manager = manager or self.connection_manager
        result = selected_manager.reconnect_connection(timeout_seconds=5.0)
        if not result.connected:
            raise RuntimeError(result.message)
        return selected_manager.get_system_diagnostics(), "Связь с Linux Gateway восстановлена; получен новый диагностический снимок."

    def _schedule_reconnect(self, reason: str, profile_id: str | None = None) -> None:
        key = self._session_key(profile_id)
        manager = self._manager_for(profile_id)
        manager.handle_connection_loss()
        self._reconnect_pending_by_profile[key] = manager.profile is not None
        self._reconnect_attempt_by_profile[key] = 0
        self._next_reconnect_by_profile[key] = time.monotonic() + 5.0
        if profile_id is not None:
            self._station_status[profile_id] = "UNKNOWN"
        self._model_for(profile_id).mark_diagnostics_stale(reason)
        self._sync_active_session_flags()
        self._append_event(reason + " Автоматическое восстановление начнётся через 5 с.", profile_id)
        self.viewChanged.emit()

    def _start_worker(self, profile_id: str | None, operation, is_reconnect: bool = False) -> None:
        key = self._session_key(profile_id)
        if key in self._busy_profiles or self._closing:
            return
        manager = self._manager_for(profile_id)
        model = self._model_for(profile_id)
        self._busy_profiles.add(key)
        self._sync_active_session_flags()
        self.viewChanged.emit()

        def run() -> None:
            try:
                if self._closing:
                    return
                result = operation()
                if self._closing:
                    manager.disconnect_from_station()
                    return
                if isinstance(result, tuple) and len(result) == 2 and isinstance(result[1], str):
                    payload, message = result
                else:
                    payload, message = result, "Диагностический снимок обновлён."
                self._operationFinished.emit(key, True, message, payload, is_reconnect, False)
            except Exception as exc:
                status_getter = getattr(manager, "get_connection_status", None)
                manager_status = status_getter() if callable(status_getter) else ("CONNECTED" if manager.is_connected else "DISCONNECTED")
                unexpected_loss = (
                    not self._reconnect_pending_by_profile.get(key, False)
                    and getattr(manager, "profile", None) is not None
                    and (model.connection_state == "CONNECTED" or manager_status in {"CONNECTED", "DEGRADED"})
                )
                try:
                    manager.handle_connection_loss()
                except Exception:
                    pass
                self._operationFinished.emit(key, False, str(exc), None, is_reconnect, unexpected_loss)

        worker = threading.Thread(target=run, name=f"dockstation-qml-worker-{key}", daemon=True)
        self._worker_threads[key] = worker
        worker.start()

    @Slot(str, bool, str, object, bool, bool)
    def _on_operation_finished(self, *args) -> None:
        if len(args) == 3:
            success, message, payload = args
            profile_id = self._selected_profile_id
            is_reconnect = self._reconnect_pending
            unexpected_loss = False
        else:
            key, success, message, payload, is_reconnect, unexpected_loss = args
            profile_id = None if key == "__default__" else key
        key = self._session_key(profile_id)
        if len(args) == 3 and self._reconnect_pending:
            self._reconnect_pending_by_profile[key] = True
            self._reconnect_attempt_by_profile.setdefault(key, self._reconnect_attempt)
            self._next_reconnect_by_profile.setdefault(key, self._next_reconnect_at)
        self._busy_profiles.discard(key)
        self._worker_threads.pop(key, None)
        manager = self._manager_for(profile_id)
        model = self._model_for(profile_id)

        if profile_id is not None:
            self._station_status[profile_id] = (
                "CONNECTED" if success and isinstance(payload, dict)
                else "DISCONNECTED" if success and payload is None
                else "UNKNOWN" if unexpected_loss or is_reconnect or model.snapshot is not None
                else "DISCONNECTED"
            )
        if success:
            if isinstance(payload, dict):
                self._reconnect_pending_by_profile[key] = False
                self._reconnect_attempt_by_profile[key] = 0
                self._next_reconnect_by_profile[key] = 0.0
                model.apply_diagnostics(payload)
                if message != "Диагностический снимок обновлён.":
                    self._append_event(message, profile_id)
            else:
                self._reconnect_pending_by_profile[key] = False
                self._reconnect_attempt_by_profile[key] = 0
                model.clear_connection_state()
                self._append_event(message, profile_id)
        else:
            model.mark_diagnostics_stale(message)
            self._append_event("Ошибка подключения/обмена: " + message, profile_id)
            if unexpected_loss:
                self._reconnect_pending_by_profile[key] = getattr(manager, "profile", None) is not None
                self._reconnect_attempt_by_profile[key] = 0
                self._next_reconnect_by_profile[key] = time.monotonic() + 5.0
                self._append_event("Автоматическое восстановление начнётся через 5 с.", profile_id)
            elif is_reconnect and self._reconnect_pending_by_profile.get(key, False):
                delays = (5.0, 10.0, 20.0, 30.0)
                attempt = self._reconnect_attempt_by_profile.get(key, 0)
                delay = delays[min(attempt, len(delays) - 1)]
                self._reconnect_attempt_by_profile[key] = attempt + 1
                self._next_reconnect_by_profile[key] = time.monotonic() + delay
                self._append_event(f"Повторная попытка подключения через {int(delay)} с.", profile_id)
        self._sync_active_session_flags()
        self.viewChanged.emit()

    def _append_event(self, message: str, profile_id: str | None = None) -> None:
        target_id = profile_id if profile_id is not None else self._selected_profile_id
        model = self._model_for(target_id)
        alias = next((item.alias for item in self._registry.profiles if item.profile_id == target_id), None)
        tagged_message = f"[{alias}] {message}" if alias else message
        self._notice = tagged_message
        model.append_client_event(tagged_message)
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

