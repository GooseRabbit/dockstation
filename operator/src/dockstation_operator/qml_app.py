"""Launch the PySide6/QML DockStation Operator prototype."""

from __future__ import annotations

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
    qml_path = Path(__file__).parent / "ui" / "qml" / "Main.qml"
    engine.load(QUrl.fromLocalFile(str(qml_path)))
    if not engine.rootObjects():
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
