"""Launch the PySide6/QML DockStation Operator prototype."""

from __future__ import annotations

import os
import sys
from pathlib import Path

from PySide6.QtCore import QUrl, qInstallMessageHandler
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine

from .ui.qml_bridge import QmlOperatorBridge


def main() -> int:
    smoke_test = "--smoke-test" in sys.argv
    qt_arguments = [argument for argument in sys.argv if argument != "--smoke-test"]
    log_path = os.environ.get("DOCKSTATION_SMOKE_LOG")

    def write_smoke_log(message: str) -> None:
        if smoke_test and log_path:
            with Path(log_path).open("a", encoding="utf-8") as stream:
                stream.write(message + "\n")

    def qt_message_handler(mode: object, context: object, message: str) -> None:
        write_smoke_log(f"Qt message: {message}")

    if smoke_test:
        if log_path:
            Path(log_path).write_text("qml_app.main entered\n", encoding="utf-8")
        qInstallMessageHandler(qt_message_handler)

    app = QGuiApplication(qt_arguments)
    write_smoke_log("QGuiApplication created")
    app.setApplicationName("DockStation Operator")
    engine = QQmlApplicationEngine()
    bridge = QmlOperatorBridge()
    engine.rootContext().setContextProperty("bridge", bridge)
    qml_warnings: list[str] = []

    def collect_qml_warnings(errors: list[object]) -> None:
        qml_warnings.extend(
            error.toString() if hasattr(error, "toString") else str(error)
            for error in errors
        )

    engine.warnings.connect(collect_qml_warnings)
    qml_path = Path(__file__).parent / "ui" / "qml" / "Main.qml"
    write_smoke_log(f"QML source exists: {qml_path.exists()} ({qml_path})")
    if getattr(sys, "frozen", False):
        bundle_root = Path(getattr(sys, "_MEIPASS", Path(__file__).parent))
        import_paths = (
            bundle_root / "PySide6" / "qml",
            bundle_root / "PySide6" / "Qt" / "qml",
        )
        for import_path in import_paths:
            if import_path.is_dir():
                engine.addImportPath(str(import_path))
                write_smoke_log(f"Added QML import path: {import_path}")
    engine.load(QUrl.fromLocalFile(str(qml_path)))
    if not engine.rootObjects():
        for warning in qml_warnings:
            write_smoke_log(warning)
        if not qml_warnings:
            write_smoke_log("QQmlApplicationEngine created no root objects and emitted no warnings.")
        bridge.shutdown()
        return 1
    if smoke_test:
        write_smoke_log(f"QML root objects loaded: {len(engine.rootObjects())}")
        # Packaging CI loads the real QML tree without opening a persistent UI.
        bridge.shutdown()
        return 0
    app.aboutToQuit.connect(bridge.shutdown)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
