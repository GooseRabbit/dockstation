"""Focused checks for QML connection-port drafts and camera preview layout."""

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


def test_qml_bridge_formats_system_metrics_and_marks_stale_snapshot():
    bridge = QmlOperatorBridge()
    try:
        bridge.model.apply_diagnostics({
            "platform": {
                "resources": {
                    "cpu_temperature": {
                        "status": "PASS", "message": "read",
                        "celsius": 44.2, "sensor_type": "cpu-thermal",
                    },
                    "memory": {
                        "status": "PASS", "message": "read",
                        "total_bytes": 1024 * 1024 * 1024,
                        "available_bytes": 256 * 1024 * 1024,
                        "used_bytes": 768 * 1024 * 1024,
                        "used_percent": 75.0,
                    },
                    "storage": {
                        "status": "PASS", "message": "read", "path": "/",
                        "total_bytes": 10 * 1024 * 1024 * 1024,
                        "available_bytes": 2 * 1024 * 1024 * 1024,
                        "used_bytes": 8 * 1024 * 1024 * 1024,
                        "used_percent": 80.0,
                    },
                    "cpu_load": {
                        "status": "PASS", "message": "read",
                        "one_minute": 0.1, "five_minutes": 0.2,
                        "fifteen_minutes": 0.3, "runnable_tasks": 1,
                        "total_tasks": 20,
                    },
                },
            },
        })
        metrics = bridge.systemMetrics
        assert metrics["cpu_temperature_value"] == "44.2 °C"
        assert metrics["memory_value"] == "75.0 %"
        assert "768.0 МиБ" in metrics["memory_detail"]
        assert metrics["storage_value"] == "80.0 %"
        assert "0.10 / 0.20 / 0.30" == metrics["load_value"]
        assert metrics["cpu_temperature_color"] == "#46A982"

        bridge.model.mark_diagnostics_stale("test disconnect")
        stale_metrics = bridge.systemMetrics
        assert stale_metrics["cpu_temperature_value"] == "44.2 °C"
        assert stale_metrics["cpu_temperature_color"] == "#D6A34A"
    finally:
        bridge.shutdown()


def test_empty_port_drafts_resolve_to_defaults_and_invalid_text_stays_invalid():
    bridge = QmlOperatorBridge()
    try:
        bridge.setSshPort("")
        bridge.setLocalPort("")
        assert bridge.sshPort == ""
        assert bridge.localPort == ""
        profile = bridge._profile_with_port_drafts()
        assert profile.ssh_port == 22
        assert profile.local_port == 18080

        bridge.setSshPort("2222")
        bridge.setLocalPort("19090")
        profile = bridge._profile_with_port_drafts()
        assert profile.ssh_port == 2222
        assert profile.local_port == 19090

        bridge.setSshPort("not-a-port")
        assert bridge._profile_with_port_drafts().ssh_port == -1
    finally:
        bridge.shutdown()


def test_qml_uses_port_placeholders_and_fixed_camera_viewport_without_auto_fullscreen():
    app = QGuiApplication.instance() or QGuiApplication([])
    engine = QQmlApplicationEngine()
    bridge = QmlOperatorBridge()
    engine.rootContext().setContextProperty("bridge", bridge)
    qml_path = Path(dockstation_operator.__file__).parent / "ui" / "qml" / "Main.qml"
    engine.load(QUrl.fromLocalFile(str(qml_path)))
    try:
        app.processEvents()
        roots = engine.rootObjects()
        assert roots
        root = roots[0]
        ssh_field = root.findChild(QObject, "connection.ssh-port-field")
        local_field = root.findChild(QObject, "connection.local-port-field")
        assert ssh_field is not None and ssh_field.property("placeholderText") == "22"
        assert local_field is not None and local_field.property("placeholderText") == "18080"
        qml_source = qml_path.read_text(encoding="utf-8")
        assert "verticalAlignment: TextInput.AlignVCenter" in qml_source
        assert root.findChild(QObject, "stations.local-profile-list") is not None

        assert root.property("currentTab") == 0
        fullscreen = root.findChild(QObject, "camera.fullscreen-window")
        assert fullscreen is not None
        assert fullscreen.property("visible") is False

        root.setProperty("currentTab", 5)
        app.processEvents()
        preview = root.findChild(QObject, "camera.preview")
        assert preview is not None
        assert round(preview.property("width")) == 960
        assert round(preview.property("height")) == 540
        assert abs(preview.property("width") / preview.property("height") - (16 / 9)) < 0.001
        assert fullscreen.property("visible") is False
    finally:
        bridge.shutdown()
        for root in engine.rootObjects():
            root.close()
        engine.deleteLater()
        app.processEvents()



def test_qml_bridge_copies_and_saves_all_retained_client_events(tmp_path: Path):
    app = QGuiApplication.instance() or QGuiApplication([])
    bridge = QmlOperatorBridge()
    try:
        for index in range(505):
            bridge.model.append_client_event(f"Событие {index}")
        expected = "\n".join(reversed(bridge.model.client_events))
        assert len(bridge.model.client_events) == 500
        assert bridge.eventsText == expected
        assert bridge.events == list(reversed(bridge.model.client_events))
        app.clipboard().clear()
        assert bridge.copyLogs() is True
        assert app.clipboard().text() == expected
        assert "500" in bridge.logsActionStatus
        destination = tmp_path / "operator-logs.txt"
        assert bridge.saveLogs(str(destination)) is True
        assert destination.read_text(encoding="utf-8").rstrip("\n") == expected
        assert "500" in bridge.logsActionStatus
        missing_directory = tmp_path / "missing"
        assert bridge.saveLogs(str(missing_directory / "logs.txt")) is False
        assert "Не удалось сохранить логи" in bridge.logsActionStatus
    finally:
        bridge.shutdown()


def test_qml_log_panel_is_fixed_height_selectable_and_keeps_recordings_separate():
    app = QGuiApplication.instance() or QGuiApplication([])
    engine = QQmlApplicationEngine()
    bridge = QmlOperatorBridge()
    for index in range(250):
        bridge.model.append_client_event(f"Длинное событие журнала {index}")
    bridge.viewChanged.emit()
    engine.rootContext().setContextProperty("bridge", bridge)
    qml_path = Path(dockstation_operator.__file__).parent / "ui" / "qml" / "Main.qml"
    engine.load(QUrl.fromLocalFile(str(qml_path)))
    try:
        app.processEvents()
        roots = engine.rootObjects()
        assert roots
        root = roots[0]
        root.setProperty("currentTab", 4)
        app.processEvents()
        panel = root.findChild(QObject, "logs.events-panel")
        scroll = root.findChild(QObject, "logs.scroll")
        log_text = root.findChild(QObject, "logs.text-area")
        copy_button = root.findChild(QObject, "logs.copy-button")
        save_button = root.findChild(QObject, "logs.save-button")
        recordings = root.findChild(QObject, "logs.recordings-panel")
        assert panel is not None and scroll is not None and log_text is not None
        assert copy_button is not None and save_button is not None and recordings is not None
        assert round(panel.property("implicitHeight")) == 420
        assert log_text.property("readOnly") is True
        assert log_text.property("selectByMouse") is True
        assert "Длинное событие журнала 0" in log_text.property("text")
        original_panel_height = panel.property("height")
        for index in range(250, 500):
            bridge.model.append_client_event(f"Дополнительное событие {index}")
        bridge.viewChanged.emit()
        app.processEvents()
        assert panel.property("height") == original_panel_height
        assert root.findChild(QObject, "logs.recordings-panel") is recordings
    finally:
        bridge.shutdown()
        for root in engine.rootObjects():
            root.close()
        engine.deleteLater()
        app.processEvents()
