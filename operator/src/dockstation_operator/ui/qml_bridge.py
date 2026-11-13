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
    _operationFinished = Signal(bool, str, object)

    def __init__(self, connection_manager: ConnectionManager | None = None):
        super().__init__()
        self.connection_manager = connection_manager or ConnectionManager()
        self.model = DiagnosticsModel()
        registry_path = station_registry_path()
        self._registry = load_registry(registry_path)
        legacy_profile = load_profile()
        # Migrate the old single-profile settings exactly once. An existing but
        # empty or damaged registry must not resurrect a profile the user removed.
        if not registry_path.exists() and legacy_profile.host and legacy_profile.username:
            try:
                migrated_registry = add_profile(self._registry, legacy_profile.host, legacy_profile)
                save_registry(migrated_registry, registry_path)
                self._registry = migrated_registry
            except (OSError, ValueError):
                # Keep the GUI available; the user can correct the profile manually.
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
        self._reconnect_pending = False
        self._reconnect_attempt = 0
        self._next_reconnect_at = 0.0
        self._notice = "Ожидание подключения."
        self._logs_action_status = ""
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
    def reconnecting(self) -> bool:
        return self._reconnect_pending

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
        """Return every event currently retained by DiagnosticsModel, newest first."""
        return list(reversed(self.model.client_events))

    @Property(str, notify=viewChanged)
    def eventsText(self) -> str:
        """Return the complete retained event buffer in the same order as the UI."""
        return "\n".join(reversed(self.model.client_events))

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
        count = len(self.model.client_events)
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
        count = len(self.model.client_events)
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
        if self._busy or self._closing:
            return
        self._reconnect_pending = False
        self._reconnect_attempt = 0
        try:
            new_profile = ConnectionProfile()
            updated_registry = add_profile(self._registry, alias, new_profile)
            save_registry(updated_registry)
            previous_id = self._selected_profile_id
            if self.connection_manager.is_connected:
                self.connection_manager.disconnect_from_station()
            if previous_id is not None and self._station_status.get(previous_id) == "CONNECTED":
                self._station_status[previous_id] = "DISCONNECTED"
            self._registry = updated_registry
            self._selected_profile_id = self._registry.selected_profile_id
            self._profile = new_profile
            self._station_alias_draft = alias.strip()
            self._ssh_port_text = ""
            self._local_port_text = ""
            self.model = DiagnosticsModel()
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
        self._reconnect_pending = False
        self._reconnect_attempt = 0
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
        self._reconnect_pending = False
        self._reconnect_attempt = 0
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
                alias = self._station_alias_draft.strip() or profile.host.strip() or "Новая станция"
                self._registry = add_profile(self._registry, alias, profile)
                self._selected_profile_id = self._registry.selected_profile_id
                self._station_alias_draft = alias
                self._station_status[self._selected_profile_id] = "UNKNOWN"
            else:
                selected_item = next(
                    item for item in self._registry.profiles
                    if item.profile_id == self._selected_profile_id
                )
                if self._station_alias_draft.strip() != selected_item.alias:
                    self._registry = rename_profile(
                        self._registry, self._selected_profile_id, self._station_alias_draft
                    )
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
        # When the link is down, keep the reconnect target aligned with the
        # profile that was just saved. An in-flight active tunnel is untouched.
        if not self.connection_manager.is_connected:
            self.connection_manager.profile = profile
        self.profileChanged.emit()
        self.viewChanged.emit()
        self._append_event(f"Локальный профиль подключения сохранён: {target}")
        self._append_event("Содержимое приватного SSH-ключа не копируется в настройки.")
        return True

    @Slot()
    def connectToStation(self) -> None:
        if self._busy or self._closing:
            return
        self._reconnect_pending = False
        self._reconnect_attempt = 0
        profile = self._profile_with_port_drafts()
        errors = validate_profile(profile, require_files=True)
        if errors:
            self._append_event("Профиль подключения не прошёл проверку: " + "; ".join(errors))
            return
        if not self.saveProfile():
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
        if self._reconnect_pending:
            if time.monotonic() >= self._next_reconnect_at:
                self._start_worker(self._reconnect_operation)
            return
        if self.connection_manager.get_connection_status() == "DEGRADED" and self.model.connection_state == "CONNECTED":
            self._schedule_reconnect("SSH-туннель неожиданно завершился.")
            return
        if self.connection_manager.is_connected:
            self.refresh()

    @Slot()
    def refresh(self) -> None:
        if self._busy or self._closing:
            return
        if not self.connection_manager.is_connected:
            if self._selected_profile_id is not None:
                self._station_status[self._selected_profile_id] = "UNKNOWN"
            self.model.mark_diagnostics_stale("Нет активного SSH/API-подключения.")
            self._append_event("Обновление пропущено: нет активного подключения.")
            return
        self._start_worker(self.connection_manager.get_system_diagnostics)

    @Slot()
    def disconnectFromStation(self) -> None:
        if self._busy or self._closing:
            return
        self._reconnect_pending = False
        self._reconnect_attempt = 0
        self._start_worker(self._disconnect_operation)

    @Slot()
    def shutdown(self) -> None:
        self._closing = True
        self._reconnect_pending = False
        worker = getattr(self, "_worker_thread", None)
        if worker is not None and worker.is_alive() and worker is not threading.current_thread():
            worker.join(timeout=15.0)
        self.connection_manager.disconnect_from_station()

    def _disconnect_operation(self) -> tuple[None, str]:
        self.connection_manager.disconnect_from_station()
        return None, "Подключение закрыто пользователем."

    def _reconnect_operation(self) -> tuple[dict[str, Any], str]:
        result = self.connection_manager.reconnect_connection(timeout_seconds=5.0)
        if not result.connected:
            raise RuntimeError(result.message)
        snapshot = self.connection_manager.get_system_diagnostics()
        return snapshot, "Связь с Linux Gateway восстановлена; получен новый диагностический снимок."

    def _schedule_reconnect(self, reason: str) -> None:
        self.connection_manager.handle_connection_loss()
        self._reconnect_pending = self.connection_manager.profile is not None
        self._reconnect_attempt = 0
        self._next_reconnect_at = time.monotonic() + 5.0
        if self._selected_profile_id is not None:
            self._station_status[self._selected_profile_id] = "UNKNOWN"
        self.model.mark_diagnostics_stale(reason)
        self._append_event(reason + " Автоматическое восстановление начнётся через 5 с.")

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
                unexpected_loss = (
                    not self._reconnect_pending
                    and self.connection_manager.profile is not None
                    and (
                        self.model.connection_state == "CONNECTED"
                        or self.connection_manager.get_connection_status() in {"CONNECTED", "DEGRADED"}
                    )
                )
                self.connection_manager.handle_connection_loss()
                if unexpected_loss:
                    self._reconnect_pending = True
                    self._reconnect_attempt = 0
                    self._next_reconnect_at = time.monotonic() + 5.0
                self._operationFinished.emit(False, str(exc), None)

        self._worker_thread = threading.Thread(target=run, name="dockstation-qml-worker", daemon=True)
        self._worker_thread.start()

    @Slot(bool, str, object)
    def _on_operation_finished(self, success: bool, message: str, payload: object) -> None:
        self._busy = False
        if self._selected_profile_id is not None:
            self._station_status[self._selected_profile_id] = (
                "CONNECTED" if success and isinstance(payload, dict)
                else "DISCONNECTED" if success and payload is None
                else "UNKNOWN"
            )
        if success:
            if isinstance(payload, dict):
                self._reconnect_pending = False
                self._reconnect_attempt = 0
                self.model.apply_diagnostics(payload)
                self._append_event(message)
            else:
                self.model.clear_connection_state()
                self._append_event(message)
        else:
            self.model.mark_diagnostics_stale(message)
            self._append_event("Ошибка подключения/обмена: " + message)
            if self._reconnect_pending:
                delays = (5.0, 10.0, 20.0, 30.0)
                delay = delays[min(self._reconnect_attempt, len(delays) - 1)]
                self._reconnect_attempt += 1
                self._next_reconnect_at = time.monotonic() + delay
                self._append_event(f"Повторная попытка подключения через {int(delay)} с.")
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

