"""Tk desktop UI for the first read-only DockStation connection slice."""

from __future__ import annotations

import queue
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from typing import Callable, TypeVar

from ..config.settings import ConnectionProfile, load_profile, save_profile, validate_profile
from ..connection.connection_manager import ConnectionManager
from ..models.diagnostics_model import DiagnosticsModel

T = TypeVar("T")


class OperatorWindow:
    POLL_INTERVAL_MS = 5000

    def __init__(self, root: tk.Tk, connection_manager: ConnectionManager | None = None):
        self.root = root
        self.connection_manager = connection_manager or ConnectionManager()
        self.model = DiagnosticsModel()
        self._busy = False
        self._closing = False
        self._closing_requested = False
        self._worker_results: queue.Queue[tuple[Callable, object, Exception | None]] = queue.Queue()

        self.root.title("DockStation Operator — диагностика подключения")
        self.root.geometry("1120x700")
        self.root.minsize(900, 600)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._configure_style()
        self._build_ui()
        self._load_saved_profile()
        self._render_all()
        self.root.after(100, self._drain_worker_results)
        self.root.after(self.POLL_INTERVAL_MS, self._scheduled_refresh)

    def _configure_style(self) -> None:
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("AppTitle.TLabel", font=("Segoe UI", 18, "bold"))
        style.configure("SubTitle.TLabel", font=("Segoe UI", 9))
        style.configure("Status.TLabel", font=("Segoe UI", 11, "bold"), padding=(10, 5))
        style.configure("Section.TLabelframe.Label", font=("Segoe UI", 10, "bold"))
        style.configure("Treeview", rowheight=22)
        style.configure("Treeview.Heading", font=("Segoe UI", 9, "bold"))

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=14)
        outer.pack(fill=tk.BOTH, expand=True)

        header = ttk.Frame(outer)
        header.pack(fill=tk.X, pady=(0, 12))
        ttk.Label(header, text="DockStation Operator", style="AppTitle.TLabel").pack(side=tk.LEFT)
        self.status_var = tk.StringVar(value="Не подключено")
        self.status_label = ttk.Label(header, textvariable=self.status_var, style="Status.TLabel")
        self.status_label.pack(side=tk.RIGHT)

        connection = ttk.LabelFrame(outer, text="Подключение к Linux VM через SSH", style="Section.TLabelframe", padding=10)
        connection.pack(fill=tk.X, pady=(0, 12))
        self.host_var = tk.StringVar()
        self.username_var = tk.StringVar()
        self.ssh_port_var = tk.StringVar(value="22")
        self.local_port_var = tk.StringVar(value="18080")
        self.key_path_var = tk.StringVar()
        self.known_hosts_var = tk.StringVar()

        ttk.Label(connection, text="Адрес Linux VM").grid(row=0, column=0, sticky=tk.W, padx=(0, 6), pady=4)
        ttk.Entry(connection, textvariable=self.host_var, width=24).grid(row=0, column=1, sticky=tk.EW, padx=(0, 12), pady=4)
        ttk.Label(connection, text="Пользователь Linux").grid(row=0, column=2, sticky=tk.W, padx=(0, 6), pady=4)
        ttk.Entry(connection, textvariable=self.username_var, width=18).grid(row=0, column=3, sticky=tk.EW, padx=(0, 12), pady=4)
        ttk.Label(connection, text="SSH порт").grid(row=0, column=4, sticky=tk.W, padx=(0, 6), pady=4)
        ttk.Entry(connection, textvariable=self.ssh_port_var, width=7).grid(row=0, column=5, sticky=tk.EW, padx=(0, 12), pady=4)
        ttk.Label(connection, text="Локальный порт").grid(row=0, column=6, sticky=tk.W, padx=(0, 6), pady=4)
        ttk.Entry(connection, textvariable=self.local_port_var, width=8).grid(row=0, column=7, sticky=tk.EW, pady=4)

        ttk.Label(connection, text="Приватный ключ").grid(row=1, column=0, sticky=tk.W, padx=(0, 6), pady=4)
        ttk.Entry(connection, textvariable=self.key_path_var).grid(row=1, column=1, columnspan=5, sticky=tk.EW, padx=(0, 8), pady=4)
        ttk.Button(connection, text="Обзор…", command=self._browse_key).grid(row=1, column=6, columnspan=2, sticky=tk.EW, pady=4)
        ttk.Label(connection, text="Файл known_hosts").grid(row=2, column=0, sticky=tk.W, padx=(0, 6), pady=4)
        ttk.Entry(connection, textvariable=self.known_hosts_var).grid(row=2, column=1, columnspan=5, sticky=tk.EW, padx=(0, 8), pady=4)
        ttk.Button(connection, text="Обзор…", command=self._browse_known_hosts).grid(row=2, column=6, columnspan=2, sticky=tk.EW, pady=4)

        actions = ttk.Frame(connection)
        actions.grid(row=3, column=0, columnspan=8, sticky=tk.EW, pady=(8, 0))
        self.connect_button = ttk.Button(actions, text="Подключить", command=self._connect)
        self.connect_button.pack(side=tk.LEFT)
        self.disconnect_button = ttk.Button(actions, text="Отключить", command=self._disconnect, state=tk.DISABLED)
        self.disconnect_button.pack(side=tk.LEFT, padx=(8, 0))
        self.refresh_button = ttk.Button(actions, text="Обновить данные", command=self._refresh)
        self.refresh_button.pack(side=tk.LEFT, padx=(8, 0))
        ttk.Button(actions, text="Сохранить профиль", command=self._save_profile).pack(side=tk.RIGHT)
        connection.columnconfigure(1, weight=2)
        connection.columnconfigure(3, weight=1)
        connection.columnconfigure(5, weight=0)
        connection.columnconfigure(7, weight=0)

        self.notebook = ttk.Notebook(outer)
        self.notebook.pack(fill=tk.BOTH, expand=True)
        self.overview_tab = ttk.Frame(self.notebook, padding=12)
        self.network_tab = ttk.Frame(self.notebook, padding=12)
        self.events_tab = ttk.Frame(self.notebook, padding=12)
        self.notebook.add(self.overview_tab, text="Обзор")
        self.notebook.add(self.network_tab, text="Сеть")
        self.notebook.add(self.events_tab, text="События")

        self._build_overview_tab()
        self._build_network_tab()
        self._build_events_tab()

        footer = ttk.Frame(outer)
        footer.pack(fill=tk.X, pady=(10, 0))
        self.footer_var = tk.StringVar(value="Ожидание подключения. Физические команды в этой версии отсутствуют.")
        ttk.Label(footer, textvariable=self.footer_var, style="SubTitle.TLabel").pack(side=tk.LEFT)
        self.last_update_var = tk.StringVar(value="Последнее обновление: —")
        ttk.Label(footer, textvariable=self.last_update_var, style="SubTitle.TLabel").pack(side=tk.RIGHT)

    def _build_overview_tab(self) -> None:
        self.stale_var = tk.StringVar(value="ДАННЫЕ НЕ ПОЛУЧЕНЫ")
        ttk.Label(self.overview_tab, textvariable=self.stale_var, font=("Segoe UI", 12, "bold")).pack(anchor=tk.W, pady=(0, 12))

        platform_frame = ttk.LabelFrame(self.overview_tab, text="Система Linux", style="Section.TLabelframe", padding=10)
        platform_frame.pack(fill=tk.X, pady=(0, 10))
        self.system_vars = {
            "api": tk.StringVar(value="Не подключено"),
            "os": tk.StringVar(value="—"),
            "kernel": tk.StringVar(value="—"),
            "architecture": tk.StringVar(value="—"),
            "uptime": tk.StringVar(value="—"),
            "summary": tk.StringVar(value="—"),
            "collected": tk.StringVar(value="—"),
            "scope": tk.StringVar(value="—"),
        }
        rows = [
            (("Gateway API", "api"), ("Операционная система", "os")),
            (("Ядро Linux", "kernel"), ("Архитектура", "architecture")),
            (("Время работы", "uptime"), ("Итог диагностики", "summary")),
            (("Время сбора (UTC)", "collected"), ("Источник данных", "scope")),
        ]
        for row_index, row_items in enumerate(rows):
            for group_index, (caption, key) in enumerate(row_items):
                label_column = group_index * 2
                ttk.Label(platform_frame, text=caption + ":", width=22).grid(
                    row=row_index, column=label_column, sticky=tk.W, padx=(0, 8), pady=4
                )
                ttk.Label(
                    platform_frame,
                    textvariable=self.system_vars[key],
                    wraplength=390,
                    justify=tk.LEFT,
                ).grid(row=row_index, column=label_column + 1, sticky=tk.W, pady=4)
        platform_frame.columnconfigure(1, weight=1)
        platform_frame.columnconfigure(3, weight=1)

        note_frame = ttk.LabelFrame(self.overview_tab, text="Границы текущей версии", style="Section.TLabelframe", padding=10)
        note_frame.pack(fill=tk.BOTH, expand=True)
        ttk.Label(
            note_frame,
            text=(
                "Linux VM: ОС/сеть; без CPU/RAM, дисков, температуры и ESP32/датчиков/приводов. Только чтение."
            ),
            justify=tk.LEFT,
            wraplength=1030,
        ).pack(anchor=tk.W)

    def _build_network_tab(self) -> None:
        interface_frame = ttk.LabelFrame(self.network_tab, text="Сетевые интерфейсы и адреса", style="Section.TLabelframe", padding=8)
        interface_frame.pack(fill=tk.X, pady=(0, 8))
        interface_table = ttk.Frame(interface_frame)
        interface_table.pack(fill=tk.X, expand=False)
        interface_table.columnconfigure(0, weight=1)
        interface_table.rowconfigure(0, weight=1)
        self.interfaces_tree = ttk.Treeview(
            interface_table,
            columns=("name", "kind", "state", "carrier", "addresses", "status"),
            show="headings",
            height=2,
        )
        for column, caption, width in (
            ("name", "Интерфейс", 110), ("kind", "Тип", 110), ("state", "Состояние", 90),
            ("carrier", "Физический линк", 115), ("addresses", "IP-адреса", 390), ("status", "Статус", 90),
        ):
            self.interfaces_tree.heading(column, text=caption)
            self.interfaces_tree.column(column, width=width, anchor=tk.W, stretch=column == "addresses")
        interface_vscroll = ttk.Scrollbar(interface_table, orient=tk.VERTICAL, command=self.interfaces_tree.yview)
        interface_hscroll = ttk.Scrollbar(interface_table, orient=tk.HORIZONTAL, command=self.interfaces_tree.xview)
        self.interfaces_tree.configure(yscrollcommand=interface_vscroll.set, xscrollcommand=interface_hscroll.set)
        self.interfaces_tree.grid(row=0, column=0, sticky="nsew")
        interface_vscroll.grid(row=0, column=1, sticky="ns")
        interface_hscroll.grid(row=1, column=0, sticky="ew")

        routes_frame = ttk.LabelFrame(self.network_tab, text="Маршруты по умолчанию", style="Section.TLabelframe", padding=8)
        routes_frame.pack(fill=tk.X, pady=(0, 8))
        routes_table = ttk.Frame(routes_frame)
        routes_table.pack(fill=tk.X, expand=False)
        routes_table.columnconfigure(0, weight=1)
        self.routes_tree = ttk.Treeview(routes_table, columns=("gateway", "interface", "metric"), show="headings", height=1)
        for column, caption, width in (("gateway", "Шлюз / семейство", 260), ("interface", "Интерфейс", 180), ("metric", "Метрика", 100)):
            self.routes_tree.heading(column, text=caption)
            self.routes_tree.column(column, width=width, anchor=tk.W)
        routes_vscroll = ttk.Scrollbar(routes_table, orient=tk.VERTICAL, command=self.routes_tree.yview)
        self.routes_tree.configure(yscrollcommand=routes_vscroll.set)
        self.routes_tree.grid(row=0, column=0, sticky="nsew")
        routes_vscroll.grid(row=0, column=1, sticky="ns")

        status_frame = ttk.LabelFrame(self.network_tab, text="DNS и SSH", style="Section.TLabelframe", padding=8)
        status_frame.pack(fill=tk.X)
        self.dns_var = tk.StringVar(value="DNS: —")
        self.ssh_var = tk.StringVar(value="SSH: —")
        ttk.Label(status_frame, textvariable=self.dns_var, wraplength=980).pack(anchor=tk.W, pady=2)
        ttk.Label(status_frame, textvariable=self.ssh_var, wraplength=980).pack(anchor=tk.W, pady=2)

    def _build_events_tab(self) -> None:
        self.events_text = tk.Text(self.events_tab, height=20, wrap=tk.WORD, state=tk.DISABLED, font=("Consolas", 9))
        scrollbar = ttk.Scrollbar(self.events_tab, orient=tk.VERTICAL, command=self.events_text.yview)
        self.events_text.configure(yscrollcommand=scrollbar.set)
        self.events_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

    def _load_saved_profile(self) -> None:
        profile = load_profile()
        self.host_var.set(profile.host)
        self.username_var.set(profile.username)
        self.ssh_port_var.set(str(profile.ssh_port))
        self.local_port_var.set(str(profile.local_port))
        self.key_path_var.set(profile.private_key_path)
        self.known_hosts_var.set(profile.known_hosts_path)

    def _profile_from_fields(self) -> ConnectionProfile:
        try:
            ssh_port = int(self.ssh_port_var.get().strip())
            local_port = int(self.local_port_var.get().strip())
        except ValueError as exc:
            raise ValueError("SSH-порт и локальный порт должны быть целыми числами.") from exc
        profile = ConnectionProfile(
            host=self.host_var.get().strip(),
            username=self.username_var.get().strip(),
            ssh_port=ssh_port,
            private_key_path=self.key_path_var.get().strip(),
            known_hosts_path=self.known_hosts_var.get().strip(),
            local_port=local_port,
        )
        errors = validate_profile(profile)
        if errors:
            raise ValueError("\n".join(errors))
        return profile

    def _save_profile(self) -> None:
        try:
            profile = self._profile_from_fields()
            target = save_profile(profile)
        except (ValueError, OSError) as exc:
            messagebox.showerror("Профиль подключения", str(exc), parent=self.root)
            return
        self.model.append_client_event(f"Профиль подключения сохранён: {target}")
        self._render_events()
        self.footer_var.set("Профиль сохранён. Содержимое приватного ключа не копируется в настройки.")

    def _browse_key(self) -> None:
        selected = filedialog.askopenfilename(title="Выбери приватный SSH-ключ", parent=self.root)
        if selected:
            self.key_path_var.set(selected)

    def _browse_known_hosts(self) -> None:
        selected = filedialog.askopenfilename(title="Выбери файл known_hosts", parent=self.root)
        if selected:
            self.known_hosts_var.set(selected)

    def _set_busy(self, busy: bool) -> None:
        self._busy = busy
        self.connect_button.configure(state=tk.DISABLED if busy else tk.NORMAL)
        self.disconnect_button.configure(state=tk.DISABLED if busy or not self.connection_manager.is_connected else tk.NORMAL)
        self.refresh_button.configure(state=tk.DISABLED if busy or not self.connection_manager.is_connected else tk.NORMAL)

    def _run_background(self, operation: Callable[[], T], success: Callable[[T], None], failure: Callable[[Exception], None]) -> None:
        if self._busy or self._closing:
            return
        self._set_busy(True)

        def worker() -> None:
            try:
                result = operation()
            except Exception as exc:  # Results are transferred to the Tk main thread through a queue.
                self._worker_results.put((failure, None, exc))
            else:
                self._worker_results.put((success, result, None))

        threading.Thread(target=worker, name="dockstation-operator-worker", daemon=True).start()

    def _drain_worker_results(self) -> None:
        if self._closing:
            return
        try:
            while True:
                callback, result, error = self._worker_results.get_nowait()
                self._set_busy(False)
                if not self._closing_requested:
                    if error is not None:
                        callback(error)
                    else:
                        callback(result)
                    self._render_all()
                if self._closing_requested:
                    self._finalize_close()
                    return
        except queue.Empty:
            pass
        self.root.after(100, self._drain_worker_results)

    def _connect(self) -> None:
        try:
            profile = self._profile_from_fields()
            save_profile(profile)
        except (ValueError, OSError) as exc:
            messagebox.showerror("Параметры подключения", str(exc), parent=self.root)
            return

        def operation():
            result = self.connection_manager.connect_to_station(profile)
            if not result.connected:
                raise RuntimeError(result.message)
            snapshot = self.connection_manager.get_system_diagnostics()
            return result.message, snapshot

        def success(value):
            message, snapshot = value
            self.model.apply_diagnostics(snapshot)
            self.model.append_client_event(message)
            self.footer_var.set(message)

        def failure(error: Exception):
            self.connection_manager.handle_connection_loss()
            self.model.mark_diagnostics_stale(str(error))
            self.model.append_client_event(f"Ошибка подключения: {error}")
            self.footer_var.set(str(error))
            messagebox.showerror("Подключение не установлено", str(error), parent=self.root)

        self._run_background(operation, success, failure)

    def _disconnect(self) -> None:
        self.connection_manager.disconnect_from_station()
        self.model.clear_connection_state()
        self.model.append_client_event("Подключение закрыто пользователем.")
        self.footer_var.set("Отключено. Последние показания сохранены и помечены как устаревшие.")
        self._render_all()

    def _refresh(self, silent: bool = False) -> None:
        if not self.connection_manager.is_connected:
            if self.connection_manager.get_connection_status() == "DEGRADED":
                self.connection_manager.handle_connection_loss()
            if not silent:
                self.footer_var.set("Нет активного SSH/API-подключения.")
            self.model.mark_diagnostics_stale("Нет активного подключения к Linux VM.")
            self._render_all()
            return

        def operation():
            return self.connection_manager.get_system_diagnostics()

        def success(snapshot):
            self.model.apply_diagnostics(snapshot)
            self.model.append_client_event("Диагностический снимок обновлён.")
            self.footer_var.set("Данные успешно получены от Linux VM.")

        def failure(error: Exception):
            self.connection_manager.handle_connection_loss()
            self.model.mark_diagnostics_stale(str(error))
            self.model.append_client_event(f"Потеря связи/ошибка API: {error}")
            self.footer_var.set("Связь нарушена. Предыдущие показания помечены как устаревшие.")

        self._run_background(operation, success, failure)

    def _scheduled_refresh(self) -> None:
        if self._closing or self._closing_requested:
            return
        if self.connection_manager.is_connected:
            self._refresh(silent=True)
        elif (
            self.model.connection_state == "CONNECTED"
            and self.connection_manager.get_tunnel_status() == "STOPPED"
        ):
            # The ssh process can exit without an API request being in flight.
            # Detect it on the UI timer and never leave the last snapshot green.
            self.connection_manager.handle_connection_loss()
            self.model.mark_diagnostics_stale("SSH-туннель неожиданно завершился.")
            self.model.append_client_event("SSH-туннель завершился; диагностические данные помечены как устаревшие.")
            self.footer_var.set("SSH-туннель завершился. Проверь сеть VM и повторно подключись.")
            self._render_all()
        self.root.after(self.POLL_INTERVAL_MS, self._scheduled_refresh)

    @staticmethod
    def _uptime_text(seconds: object) -> str:
        if not isinstance(seconds, (int, float)):
            return "Недоступно"
        total = int(seconds)
        days, remainder = divmod(total, 86400)
        hours, remainder = divmod(remainder, 3600)
        minutes, _ = divmod(remainder, 60)
        return f"{days} дн. {hours:02d}:{minutes:02d}"

    def _render_all(self) -> None:
        connected = self.connection_manager.is_connected
        snapshot = self.model.snapshot
        if connected and not self.model.stale:
            self.status_var.set("ПОДКЛЮЧЕНО · SSH + API")
        elif self.model.connection_state == "DEGRADED" and self.model.stale and snapshot is not None:
            self.status_var.set("СВЯЗЬ ПОТЕРЯНА · ДАННЫЕ УСТАРЕЛИ")
        elif self.model.connection_state == "DISCONNECTED":
            self.status_var.set("ОТКЛЮЧЕНО")
        elif connected:
            self.status_var.set("КАНАЛ ЕСТЬ · НЕТ СНИМКА")
        else:
            self.status_var.set("НЕ ПОДКЛЮЧЕНО")
        self._set_busy(self._busy)
        self.system_vars["api"].set("Доступен · read-only API v1" if connected else "Недоступен / нет SSH-туннеля")
        if snapshot is None:
            self.stale_var.set("ДАННЫЕ НЕ ПОЛУЧЕНЫ")
            self.last_update_var.set("Последнее обновление: —")
            self._render_empty_network()
            return

        self.stale_var.set("УСТАРЕВШИЕ ДАННЫЕ · " + self.model.stale_reason if self.model.stale else "ДАННЫЕ АКТУАЛЬНЫ")
        platform_data = snapshot.get("platform", {})
        os_data = platform_data.get("os", {}) if isinstance(platform_data, dict) else {}
        uptime = platform_data.get("uptime", {}) if isinstance(platform_data, dict) else {}
        os_name = self._value(os_data.get("name"))
        os_version = os_data.get("version")
        self.system_vars["os"].set(f"{os_name} · {os_version}" if os_version else os_name)
        self.system_vars["kernel"].set(self._value(platform_data.get("kernel")))
        self.system_vars["architecture"].set(f"{self._value(platform_data.get('architecture'))} / {self._value(platform_data.get('word_size_bits'))} бит")
        self.system_vars["uptime"].set(self._uptime_text(uptime.get("seconds") if isinstance(uptime, dict) else None))
        summary = snapshot.get("summary", {})
        self.system_vars["summary"].set(f"{self._value(summary.get('status'))}: {self._value(summary.get('message'))}" if isinstance(summary, dict) else "Недоступно")
        self.system_vars["collected"].set(self._value(snapshot.get("collected_at_utc")))
        scope = snapshot.get("scope", {})
        self.system_vars["scope"].set(self._value(snapshot.get("source")) + (" (без ESP32 и физических датчиков)" if isinstance(scope, dict) and scope.get("hardware_controller_connected") is False else ""))
        received = self.model.received_at_utc.astimezone().strftime("%Y-%m-%d %H:%M:%S") if self.model.received_at_utc else "—"
        self.last_update_var.set("Получено: " + received)
        self._render_network(snapshot)
        self._render_events()

    @staticmethod
    def _value(value: object) -> str:
        return DiagnosticsModel.format_diagnostic_value(value)

    def _render_empty_network(self) -> None:
        for tree in (self.interfaces_tree, self.routes_tree):
            for item in tree.get_children():
                tree.delete(item)
        self.dns_var.set("DNS: данные не получены")
        self.ssh_var.set("SSH: данные не получены")
        self._render_events()

    def _render_network(self, snapshot: dict) -> None:
        network = snapshot.get("network", {})
        interfaces_section = network.get("interfaces", {}) if isinstance(network, dict) else {}
        for item in self.interfaces_tree.get_children():
            self.interfaces_tree.delete(item)
        interfaces = interfaces_section.get("interfaces", []) if isinstance(interfaces_section, dict) else []
        for interface in interfaces:
            addresses = ", ".join(address.get("address", "") for address in interface.get("addresses", []))
            carrier = interface.get("carrier")
            carrier_text = "Да" if carrier is True else "Нет" if carrier is False else "Неизвестно"
            self.interfaces_tree.insert("", tk.END, values=(
                interface.get("name", "—"), interface.get("kind", "—"), interface.get("oper_state") or "—",
                carrier_text, addresses or "—", interface.get("status", "UNKNOWN"),
            ))
        if not interfaces:
            self.interfaces_tree.insert("", tk.END, values=("Интерфейсы не обнаружены", "—", "—", "—", "—", "UNAVAILABLE"))

        routes_section = network.get("default_routes", {}) if isinstance(network, dict) else {}
        for item in self.routes_tree.get_children():
            self.routes_tree.delete(item)
        routes = routes_section.get("routes", []) if isinstance(routes_section, dict) else []
        for route in routes:
            gateway = f"{route.get('family', '—')} · {route.get('gateway') or 'on-link'}"
            self.routes_tree.insert("", tk.END, values=(gateway, route.get("interface") or "—", route.get("metric") if route.get("metric") is not None else "—"))
        if not routes:
            self.routes_tree.insert("", tk.END, values=("Маршрут по умолчанию не обнаружен", "—", "—"))

        dns = network.get("dns", {}) if isinstance(network, dict) else {}
        dns_addresses = ", ".join(dns.get("addresses", [])) if isinstance(dns, dict) else ""
        dns_hostname = dns.get("hostname") if isinstance(dns, dict) else None
        dns_target = f" · {dns_hostname}" if dns_hostname else ""
        self.dns_var.set(f"DNS: {self._value(dns.get('status'))} · {self._value(dns.get('message'))}" + dns_target + (f" · {dns_addresses}" if dns_addresses else ""))
        ssh = network.get("ssh", {}) if isinstance(network, dict) else {}
        active_units = ", ".join(ssh.get("active_units", [])) if isinstance(ssh, dict) else ""
        self.ssh_var.set(f"SSH: {self._value(ssh.get('status'))} · {self._value(ssh.get('message'))}" + (f" · {active_units}" if active_units else ""))

    def _render_events(self) -> None:
        self.events_text.configure(state=tk.NORMAL)
        self.events_text.delete("1.0", tk.END)
        self.events_text.insert(tk.END, "\n".join(self.model.client_events))
        self.events_text.see(tk.END)
        self.events_text.configure(state=tk.DISABLED)

    def _on_close(self) -> None:
        if self._busy:
            self._closing_requested = True
            self.footer_var.set("Завершается текущая сетевая операция; приложение закроется сразу после неё.")
            return
        self._finalize_close()

    def _finalize_close(self) -> None:
        self._closing = True
        try:
            self.connection_manager.disconnect_from_station()
        finally:
            self.root.destroy()
