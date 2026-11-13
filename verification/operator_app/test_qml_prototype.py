"""Headless smoke test for the seven-tab PySide6/QML Operator prototype."""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("QT_QUICK_CONTROLS_STYLE", "Basic")

from PySide6.QtCore import QObject, QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine

import dockstation_operator
from dockstation_operator.ui.qml_bridge import QmlOperatorBridge


def test_qml_prototype_loads_with_the_seven_approved_tabs():
    app = QGuiApplication.instance() or QGuiApplication([])
    engine = QQmlApplicationEngine()
    bridge = QmlOperatorBridge()
    engine.rootContext().setContextProperty("bridge", bridge)
    qml_path = Path(dockstation_operator.__file__).parent / "ui" / "qml" / "Main.qml"

    engine.load(QUrl.fromLocalFile(str(qml_path)))

    try:
        app.processEvents()
        roots = engine.rootObjects()
        assert roots, "QML engine did not create the application window."
        page_names = [
            "page.connection",
            "page.climate",
            "page.sensors",
            "page.network",
            "page.logs",
            "page.camera",
            "page.system",
        ]
        for page_index, page_name in enumerate(page_names):
            page = roots[0].findChild(QObject, page_name)
            assert page is not None, f"Missing page container: {page_name}"
            assert page.property("padding") == 28
            roots[0].setProperty("currentTab", page_index)
            app.processEvents()
            assert page.property("contentWidth") > 0
        roots[0].setProperty("currentTab", 0)
        app.processEvents()
        cover_status = roots[0].findChild(QObject, "sensor.cover-status")
        assert cover_status is not None, "Lid position must have its own panel."
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
            "Система",
        ]
        assert bridge.connected is False
        initial_metrics = bridge.systemMetrics
        assert initial_metrics["cpu_temperature_value"] == "Недоступно"
        assert initial_metrics["memory_value"] == "Недоступно"
        assert initial_metrics["storage_value"] == "Недоступно"
        assert initial_metrics["load_value"] == "Недоступно"
        roots[0].setProperty("currentTab", 6)
        app.processEvents()
        assert roots[0].findChild(QObject, "page.system") is not None
        for panel_name in (
            "system.cpu-temperature",
            "system.memory",
            "system.storage",
            "system.load-average",
            "system.platform-info",
        ):
            assert roots[0].findChild(QObject, panel_name) is not None, panel_name
        assert bridge.freshnessText == "Данные ещё не получены"
    finally:
        bridge.shutdown()
        for root in engine.rootObjects():
            root.close()
        engine.deleteLater()
        app.processEvents()
