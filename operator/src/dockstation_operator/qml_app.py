"""Launch the PySide6/QML DockStation Operator prototype."""

from __future__ import annotations

import os
import sys
from pathlib import Path

from PySide6.QtCore import QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine

from .ui.qml_bridge import QmlOperatorBridge


def main() -> int:
    smoke_test = "--smoke-test" in sys.argv
    qt_arguments = [argument for argument in sys.argv if argument != "--smoke-test"]
    app = QGuiApplication(qt_arguments)
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
    engine.load(QUrl.fromLocalFile(str(qml_path)))
    if not engine.rootObjects():
        if smoke_test:
            log_path = os.environ.get("DOCKSTATION_SMOKE_LOG")
            if log_path:
                details = [f"QML source: {qml_path}", *qml_warnings]
                if not qml_warnings:
                    details.append("QQmlApplicationEngine created no root objects and emitted no warnings.")
                Path(log_path).write_text("\n".join(details) + "\n", encoding="utf-8")
        bridge.shutdown()
        return 1
    if smoke_test:
        # Packaging CI loads the real QML tree without opening a persistent UI.
        bridge.shutdown()
        return 0
    app.aboutToQuit.connect(bridge.shutdown)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
