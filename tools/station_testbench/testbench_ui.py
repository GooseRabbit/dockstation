# DockStation StationStateManager testbench UI (Windows / Python 3 / Tkinter).
# The UI talks to testbench_engine.exe, which calls selected real C++ core functions.

from __future__ import annotations

import os
import queue
import subprocess
import sys
import tkinter as tk
from tkinter import messagebox, ttk
from pathlib import Path


HERE = Path(__file__).resolve().parent
ENGINE = HERE / "testbench_engine.exe"

STATE_RU = {
    "POWERED_OFF": "Выключена",
    "STARTUP": "Запуск",
    "SELF_TEST": "Самопроверка",
    "WAITING": "Ожидание",
    "UNKNOWN": "Неизвестно",
}


class EngineClient:
    def __init__(self, executable: Path) -> None:
        self.process = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )

    def request(self, command: str) -> list[list[str]]:
        if self.process.poll() is not None:
            stderr = ""
            if self.process.stderr:
                stderr = self.process.stderr.read().strip()
            raise RuntimeError("Движок тестового стенда завершился. " + stderr)
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

        result: list[list[str]] = []
        while True:
            line = self.process.stdout.readline()
            if line == "":
                raise RuntimeError("Движок завершил связь до ответа на команду.")
            line = line.rstrip("\r\n")
            if line == "END":
                return result
            result.append(line.split("\t"))

    def close(self) -> None:
        if self.process.poll() is None:
            try:
                self.request("QUIT")
            except Exception:
                pass
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()


class TestBenchApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("DockStation — StationStateManager TestBench v0.2")
        self.root.geometry("1320x820")
        self.root.minsize(1050, 620)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        try:
            self.engine = EngineClient(ENGINE)
        except OSError as exc:
            messagebox.showerror("Не удалось запустить движок", str(exc))
            raise

        self.configuration_validated = tk.BooleanVar(value=True)
        self.diagnostic_report_complete = tk.BooleanVar(value=True)
        self.critical_checks_passed = tk.BooleanVar(value=True)
        self.safe_initial_condition = tk.BooleanVar(value=True)
        self.recovery_status = tk.StringVar(value="NOT_REQUIRED")

        self.state_value = tk.StringVar(value="Загрузка…")
        self.revision_value = tk.StringVar(value="—")
        self.transition_value = tk.StringVar(value="—")
        self.result_value = tk.StringVar(value="Ожидание команды")
        self.result_detail = tk.StringVar(value="")

        self._build_layout()
        self.refresh_state(write_log=False)
        self.log("Стенд запущен. Используется имитатор хранилища в оперативной памяти.")
        self.log("Физическое оборудование не подключается и не управляется.")

    def _build_layout(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        main = ttk.Frame(self.root, padding=10)
        main.grid(row=0, column=0, sticky="nsew")
        main.columnconfigure(0, weight=0, minsize=355)
        main.columnconfigure(1, weight=1)
        main.rowconfigure(0, weight=1)

        # Scrollable left control panel. The scrollbar belongs to the controls column,
        # so every button remains reachable on shorter displays.
        left_shell = ttk.Frame(main, width=355)
        left_shell.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        left_shell.grid_propagate(False)
        left_shell.columnconfigure(0, weight=1)
        left_shell.rowconfigure(0, weight=1)

        self.controls_canvas = tk.Canvas(left_shell, highlightthickness=0, borderwidth=0)
        self.controls_scrollbar = ttk.Scrollbar(
            left_shell, orient="vertical", command=self.controls_canvas.yview
        )
        self.controls_canvas.configure(yscrollcommand=self.controls_scrollbar.set)
        self.controls_canvas.grid(row=0, column=0, sticky="nsew")
        self.controls_scrollbar.grid(row=0, column=1, sticky="ns")

        self.controls = ttk.Frame(self.controls_canvas, padding=(2, 2, 8, 8))
        self.controls_window = self.controls_canvas.create_window(
            (0, 0), window=self.controls, anchor="nw"
        )
        self.controls.bind("<Configure>", self._update_scroll_region)
        self.controls_canvas.bind("<Configure>", self._fit_controls_width)
        self.controls_canvas.bind_all("<MouseWheel>", self._on_mousewheel)
        self.controls_canvas.bind_all("<Button-4>", self._on_mousewheel_linux)
        self.controls_canvas.bind_all("<Button-5>", self._on_mousewheel_linux)

        self._build_controls()
        self._build_dashboard(main)

    def _update_scroll_region(self, _event: tk.Event) -> None:
        self.controls_canvas.configure(scrollregion=self.controls_canvas.bbox("all"))

    def _fit_controls_width(self, event: tk.Event) -> None:
        self.controls_canvas.itemconfigure(self.controls_window, width=event.width)

    def _on_mousewheel(self, event: tk.Event) -> None:
        widget = self.root.winfo_containing(event.x_root, event.y_root)
        if widget is None:
            return
        if self._is_descendant(widget, self.controls_canvas):
            self.controls_canvas.yview_scroll(int(-event.delta / 120), "units")

    def _on_mousewheel_linux(self, event: tk.Event) -> None:
        widget = self.root.winfo_containing(event.x_root, event.y_root)
        if widget is not None and self._is_descendant(widget, self.controls_canvas):
            self.controls_canvas.yview_scroll(-1 if event.num == 4 else 1, "units")

    @staticmethod
    def _is_descendant(widget: tk.Widget, parent: tk.Widget) -> bool:
        current: tk.Widget | None = widget
        while current is not None:
            if current == parent:
                return True
            current = current.master
        return False

    def _build_controls(self) -> None:
        ttk.Label(self.controls, text="УПРАВЛЕНИЕ СЦЕНАРИЕМ", font=("Segoe UI", 11, "bold")).pack(
            anchor="w", pady=(0, 8)
        )

        startup = ttk.LabelFrame(self.controls, text="Штатный запуск", padding=8)
        startup.pack(fill="x", pady=5)
        ttk.Button(startup, text="1. Запросить запуск", command=lambda: self.run_action("STARTUP")).pack(
            fill="x", pady=3
        )
        ttk.Checkbutton(
            startup,
            text="Конфигурация проверена",
            variable=self.configuration_validated,
        ).pack(anchor="w", pady=2)
        ttk.Button(
            startup,
            text="2. Завершить инициализацию",
            command=self.initialize_station,
        ).pack(fill="x", pady=3)

        selftest = ttk.LabelFrame(self.controls, text="Самопроверка", padding=8)
        selftest.pack(fill="x", pady=5)
        ttk.Checkbutton(
            selftest,
            text="Получен полный отчёт диагностики",
            variable=self.diagnostic_report_complete,
        ).pack(anchor="w", pady=2)
        ttk.Checkbutton(
            selftest,
            text="Все критические проверки пройдены",
            variable=self.critical_checks_passed,
        ).pack(anchor="w", pady=2)
        ttk.Checkbutton(
            selftest,
            text="Подтверждено безопасное исходное состояние",
            variable=self.safe_initial_condition,
        ).pack(anchor="w", pady=2)
        ttk.Label(selftest, text="Результат проверки необходимости восстановления:").pack(
            anchor="w", pady=(6, 2)
        )
        for label, value in (
            ("Неизвестно (блокирует запуск)", "UNKNOWN"),
            ("Восстановление требуется", "REQUIRED"),
            ("Восстановление не требуется", "NOT_REQUIRED"),
        ):
            ttk.Radiobutton(
                selftest, text=label, variable=self.recovery_status, value=value
            ).pack(anchor="w", pady=1)
        ttk.Button(
            selftest,
            text="3. Завершить самопроверку",
            command=self.complete_self_test,
        ).pack(fill="x", pady=(7, 3))

        other = ttk.LabelFrame(self.controls, text="Дополнительные проверки", padding=8)
        other.pack(fill="x", pady=5)
        ttk.Button(
            other,
            text="Повторить последнее событие (F03)",
            command=lambda: self.run_action("DUPLICATE"),
        ).pack(fill="x", pady=3)
        ttk.Button(
            other,
            text="Проверить конфликт event_id (F03)",
            command=lambda: self.run_action("CONFLICT"),
        ).pack(fill="x", pady=3)
        ttk.Button(
            other,
            text="Проверить устаревший sequence (F04)",
            command=lambda: self.run_action("STALE"),
        ).pack(fill="x", pady=3)
        ttk.Button(
            other,
            text="Проверить повтор sequence (F04)",
            command=lambda: self.run_action("UNORDERED"),
        ).pack(fill="x", pady=3)
        ttk.Button(
            other,
            text="Проверить запрос выключения",
            command=lambda: self.run_action("SHUTDOWN"),
        ).pack(fill="x", pady=3)
        ttk.Label(
            other,
            text=(
                "Переход выключения пока не утверждён в ядре. Эта кнопка проверяет, "
                "что неподдерживаемая команда отклоняется, а состояние не меняется."
            ),
            wraplength=300,
            justify="left",
        ).pack(anchor="w", pady=(2, 4))
        ttk.Button(other, text="Обновить состояние и историю", command=self.refresh_state).pack(
            fill="x", pady=3
        )

        reset = ttk.LabelFrame(self.controls, text="Сброс теста", padding=8)
        reset.pack(fill="x", pady=5)
        ttk.Button(
            reset,
            text="Новый тестовый прогон",
            command=self.reset_test_run,
        ).pack(fill="x", pady=3)
        ttk.Label(
            reset,
            text=(
                "Сбрасывает только оперативный имитатор: состояние, revision и историю "
                "текущего прогона. Это не команда выключения реальной станции."
            ),
            wraplength=300,
            justify="left",
        ).pack(anchor="w", pady=(2, 4))
        ttk.Button(
            reset,
            text="Очистить экранные показания",
            command=self.clear_screen_readings,
        ).pack(fill="x", pady=3)
        ttk.Label(
            reset,
            text=(
                "Очищает только журнал сообщений на экране. Логическое состояние "
                "и сохранённая в имитаторе история переходов остаются без изменений."
            ),
            wraplength=300,
            justify="left",
        ).pack(anchor="w", pady=(2, 4))

        note = ttk.LabelFrame(self.controls, text="Границы стенда", padding=8)
        note.pack(fill="x", pady=5)
        ttk.Label(
            note,
            text=(
                "Подключены реальные функции F01–F05, F10, F16 и F17. "
                "Хранилище и публикация событий имитируются в памяти. "
                "Команды не управляют приводами, реле, зарядкой или питанием."
            ),
            wraplength=300,
            justify="left",
        ).pack(anchor="w")

    def _build_dashboard(self, parent: ttk.Frame) -> None:
        right = ttk.Frame(parent)
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(2, weight=1)
        right.rowconfigure(4, weight=1)

        state_card = ttk.LabelFrame(right, text="Текущее состояние станции", padding=12)
        state_card.grid(row=0, column=0, sticky="ew", pady=(0, 8))
        state_card.columnconfigure(0, weight=1)
        ttk.Label(state_card, textvariable=self.state_value, font=("Segoe UI", 21, "bold")).grid(
            row=0, column=0, sticky="w"
        )
        metadata = ttk.Frame(state_card)
        metadata.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        ttk.Label(metadata, text="Revision:").pack(side="left")
        ttk.Label(metadata, textvariable=self.revision_value, font=("Consolas", 10, "bold")).pack(
            side="left", padx=(4, 20)
        )
        ttk.Label(metadata, text="Последний transition_id:").pack(side="left")
        ttk.Label(metadata, textvariable=self.transition_value, font=("Consolas", 10)).pack(
            side="left", padx=4
        )

        result = ttk.LabelFrame(right, text="Последний отклик ядра", padding=10)
        result.grid(row=1, column=0, sticky="ew", pady=(0, 8))
        ttk.Label(result, textvariable=self.result_value, font=("Segoe UI", 11, "bold")).pack(anchor="w")
        ttk.Label(result, textvariable=self.result_detail, wraplength=850, justify="left").pack(
            anchor="w", pady=(3, 0)
        )

        history_frame = ttk.LabelFrame(right, text="История подтверждённых переходов (F17)", padding=8)
        history_frame.grid(row=2, column=0, sticky="nsew", pady=(0, 8))
        history_frame.rowconfigure(0, weight=1)
        history_frame.columnconfigure(0, weight=1)
        columns = ("revision", "from", "to", "event_id", "transition_id")
        self.history = ttk.Treeview(history_frame, columns=columns, show="headings", height=9)
        headings = {
            "revision": "Revision",
            "from": "Из состояния",
            "to": "В состояние",
            "event_id": "Event ID",
            "transition_id": "Transition ID",
        }
        widths = {"revision": 75, "from": 150, "to": 150, "event_id": 180, "transition_id": 200}
        for col in columns:
            self.history.heading(col, text=headings[col])
            self.history.column(col, width=widths[col], minwidth=65, stretch=(col != "revision"))
        history_scroll = ttk.Scrollbar(history_frame, orient="vertical", command=self.history.yview)
        self.history.configure(yscrollcommand=history_scroll.set)
        self.history.grid(row=0, column=0, sticky="nsew")
        history_scroll.grid(row=0, column=1, sticky="ns")

        log_frame = ttk.LabelFrame(right, text="Журнал ответов тестового стенда", padding=8)
        log_frame.grid(row=4, column=0, sticky="nsew")
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)
        self.activity = tk.Text(
            log_frame,
            height=8,
            wrap="word",
            state="disabled",
            font=("Consolas", 9),
        )
        log_scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.activity.yview)
        self.activity.configure(yscrollcommand=log_scroll.set)
        self.activity.grid(row=0, column=0, sticky="nsew")
        log_scroll.grid(row=0, column=1, sticky="ns")

    def log(self, message: str) -> None:
        self.activity.configure(state="normal")
        self.activity.insert("end", message + "\n")
        self.activity.see("end")
        self.activity.configure(state="disabled")

    def _translate_state(self, state: str) -> str:
        return f"{STATE_RU.get(state, state)}  /  {state}"

    def _handle_response(self, rows: list[list[str]], log_action: bool = True) -> None:
        action_row = next((row for row in rows if row and row[0] == "ACTION"), None)
        if action_row:
            status = action_row[1] if len(action_row) > 1 else "UNKNOWN"
            reason = action_row[2] if len(action_row) > 2 else ""
            detail = action_row[3] if len(action_row) > 3 else ""
            translated = {
                "COMMITTED": "Переход подтверждён и записан",
                "REJECTED": "Запрос отклонён без изменения состояния",
                "DUPLICATE_DETECTED": "F03: повтор события распознан",
                "CONFLICT_DETECTED": "F03: конфликт идентификатора распознан",
                "STALE_DETECTED": "F04: устаревший номер распознан",
                "UNORDERED_DETECTED": "F04: порядок событий не подтверждён",
                "CHECK_UNAVAILABLE": "Проверка недоступна",
                "CHECK_FAILED": "Проверка вернула неожиданный результат",
                "RESET": "Тестовый прогон сброшен",
                "DUPLICATE_EVENT": "Повторное событие",
                "EVENT_CONFLICT": "Конфликт event_id",
                "TRANSITION_ID_CONFLICT": "Конфликт transition_id",
                "SNAPSHOT_CHANGED": "Снимок состояния изменился",
                "INVALID_PLAN": "Некорректный план перехода",
                "INVALID_EVIDENCE": "Не прошла проверка события или условий",
                "REPOSITORY_ERROR": "Ошибка имитатора хранилища",
                "ERROR": "Ошибка тестового стенда",
            }
            self.result_value.set(translated.get(status, status))
            self.result_detail.set(" · ".join(part for part in (reason, detail) if part))
            if log_action:
                self.log(f"{status}: {reason} — {detail}" if reason else f"{status}: {detail}")

        snapshot_row = next((row for row in rows if row and row[0] == "SNAPSHOT"), None)
        if snapshot_row and len(snapshot_row) >= 5:
            self.state_value.set(self._translate_state(snapshot_row[2]))
            self.revision_value.set(snapshot_row[3])
            self.transition_value.set(snapshot_row[4] or "—")

        for item in self.history.get_children():
            self.history.delete(item)
        for row in rows:
            if row and row[0] == "ROW" and len(row) >= 6:
                self.history.insert(
                    "",
                    "end",
                    values=(row[1], self._translate_state(row[2]), self._translate_state(row[3]), row[4], row[5]),
                )

    def _request(self, command: str, log_action: bool = True) -> None:
        try:
            rows = self.engine.request(command)
        except Exception as exc:  # Keep the UI alive and show a useful error.
            self.result_value.set("Ошибка связи с движком")
            self.result_detail.set(str(exc))
            self.log("ОШИБКА: " + str(exc))
            return
        self._handle_response(rows, log_action=log_action)

    def run_action(self, command: str) -> None:
        self._request(command)

    def initialize_station(self) -> None:
        value = "1" if self.configuration_validated.get() else "0"
        self._request("INITIALIZE|" + value)

    def complete_self_test(self) -> None:
        values = [
            "1" if self.diagnostic_report_complete.get() else "0",
            "1" if self.critical_checks_passed.get() else "0",
            "1" if self.safe_initial_condition.get() else "0",
            self.recovery_status.get(),
        ]
        self._request("SELF_TEST|" + "|".join(values))

    def refresh_state(self, write_log: bool = True) -> None:
        self._request("STATE", log_action=False)
        if write_log:
            self.log("Состояние и история перечитаны через F16/F17.")

    def reset_test_run(self) -> None:
        approved = messagebox.askyesno(
            "Начать новый прогон?",
            "Сбросить состояние, revision и историю только текущего имитируемого прогона?\n\n"
            "Это не выключает реальную станцию и не затрагивает файлы проекта.",
        )
        if approved:
            self._request("RESET")

    def clear_screen_readings(self) -> None:
        self.activity.configure(state="normal")
        self.activity.delete("1.0", "end")
        self.activity.configure(state="disabled")
        self.result_value.set("Экранный журнал очищен")
        self.result_detail.set("Текущее состояние и история переходов сохранены без изменений.")
        self.log("Очищены только сообщения на экране; состояние и F17-история не сброшены.")

    def on_close(self) -> None:
        self.engine.close()
        self.root.destroy()


def main() -> int:
    if not ENGINE.exists():
        messagebox.showerror(
            "Не найден движок",
            "Не найден testbench_engine.exe. Запусти START_TESTBENCH.bat, "
            "чтобы собрать его из исходников проекта.",
        )
        return 2
    root = tk.Tk()
    try:
        TestBenchApp(root)
    except Exception:
        root.destroy()
        return 1
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
