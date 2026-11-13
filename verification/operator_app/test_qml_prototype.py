"""Headless smoke test for the six-tab PySide6/QML Operator prototype."""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("QT_QUICK_CONTROLS_STYLE", "Basic")

from PySide6.QtCore import QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine

import dockstation_operator
from dockstation_operator.ui.qml_bridge import QmlOperatorBridge


def test_qml_prototype_loads_with_the_six_approved_tabs():
    app = QGuiApplication.instance() or QGuiApplication([])
    engine = QQmlApplicationEngine()
    bridge = QmlOperatorBridge()
    engine.rootContext().setContextProperty("bridge", bridge)
    qml_path = Path(dockstation_operator.__file__).parent / "ui" / "qml" / "Main.qml"

    engine.load(QUrl.fromLocalFile(str(qml_path)))

    try:
        roots = engine.rootObjects()
        assert roots, "QML engine did not create the application window."
        navigation = roots[0].property("navigation")
        if hasattr(navigation, "toVariant"):
            navigation = navigation.toVariant()
        labels = [item["label"] for item in navigation]
        assert labels == [
            "Подключение",
            "Климат",
            "Датчики",
            "Сеть",
            "Логи",
            "Камера",
        ]
        assert bridge.connected is False
        assert bridge.freshnessText == "Данные ещё не получены"
    finally:
        bridge.shutdown()
        for root in engine.rootObjects():
            root.close()
        engine.deleteLater()
        app.processEvents()
