"""Focused checks for QML connection-port drafts and camera preview layout."""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("QT_QUICK_CONTROLS_STYLE", "Basic")

from PySide6.QtCore import QObject, QUrl, Qt
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine

import dockstation_operator
from dockstation_operator.ui.qml_bridge import QmlOperatorBridge


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


def test_qml_uses_port_placeholders_and_a_16_by_9_fullscreen_camera_view():
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
        assert ssh_field.property("verticalAlignment") == Qt.AlignVCenter
        assert local_field.property("verticalAlignment") == Qt.AlignVCenter

        root.setProperty("currentTab", 5)
        app.processEvents()
        preview = root.findChild(QObject, "camera.preview")
        fullscreen = root.findChild(QObject, "camera.fullscreen-window")
        assert preview is not None and fullscreen is not None
        assert preview.property("height") > 0
        assert abs(preview.property("width") / preview.property("height") - (16 / 9)) < 0.02
    finally:
        bridge.shutdown()
        for root in engine.rootObjects():
            root.close()
        engine.deleteLater()
        app.processEvents()
