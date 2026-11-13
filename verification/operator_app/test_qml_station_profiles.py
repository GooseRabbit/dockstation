from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("QT_QUICK_CONTROLS_STYLE", "Basic")

from PySide6.QtCore import QObject, QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine

import dockstation_operator
from dockstation_operator.config.settings import ConnectionProfile
from dockstation_operator.config import station_registry
from dockstation_operator.ui import qml_bridge


class InertConnectionManager:
    is_connected = False

    def __init__(self):
        self.profile = None
        self.disconnect_count = 0

    def disconnect_from_station(self):
        self.disconnect_count += 1
        self.profile = None

    def get_connection_status(self):
        return "CONNECTED" if self.is_connected else "DISCONNECTED"

    def handle_connection_loss(self):
        self.is_connected = False


def test_qml_bridge_manages_multiple_local_profiles_and_personal_aliases(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    bridge = qml_bridge.QmlOperatorBridge(connection_manager=InertConnectionManager())
    try:
        bridge.addStationProfile("Дом")
        first_id = bridge.selectedProfileId
        assert first_id
        assert bridge.stationAlias == "Дом"

        bridge.setHost("192.0.2.10")
        bridge.setUsername("operator")
        bridge.setStationAlias("Дом · основная")
        assert bridge.saveProfile() is True

        bridge.addStationProfile("Склад")
        second_id = bridge.selectedProfileId
        assert second_id and second_id != first_id
        assert len(bridge.stationProfiles) == 2
        assert [item["alias"] for item in bridge.stationProfiles] == ["Дом · основная", "Склад"]
        assert all(item["status"] == "UNKNOWN" for item in bridge.stationProfiles)

        bridge._station_status[first_id] = "CONNECTED"
        first_status = next(item for item in bridge.stationProfiles if item["profile_id"] == first_id)
        assert first_status["status_label"] == "Подключено"
        assert first_status["status_color"] == "success"

        bridge._station_status[second_id] = "DISCONNECTED"
        second_status = next(item for item in bridge.stationProfiles if item["profile_id"] == second_id)
        assert second_status["status_label"] == "Отключено"
        assert second_status["status_color"] == "danger"

        bridge.selectStationProfile(first_id)
        assert bridge.stationAlias == "Дом · основная"
        assert bridge.host == "192.0.2.10"

        bridge.removeSelectedStation()
        assert len(bridge.stationProfiles) == 1
        assert bridge.stationProfiles[0]["profile_id"] == second_id
        assert bridge.selectedProfileId == second_id
    finally:
        bridge.shutdown()


def test_existing_empty_registry_does_not_resurrect_legacy_single_profile(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    station_registry.save_registry(station_registry.StationRegistry(), registry_file)
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(
        qml_bridge,
        "load_profile",
        lambda: ConnectionProfile(host="192.0.2.99", username="operator"),
    )

    bridge = qml_bridge.QmlOperatorBridge(connection_manager=InertConnectionManager())
    try:
        assert bridge.stationProfiles == []
        assert bridge.selectedProfileId == ""
    finally:
        bridge.shutdown()



def test_qml_bridge_schedules_read_only_reconnect_after_unexpected_loss(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    class RecoverableConnectionManager(InertConnectionManager):
        def __init__(self):
            self.profile = ConnectionProfile(host="192.0.2.10", username="operator")
            self.loss_handled = False

        def handle_connection_loss(self):
            self.loss_handled = True

        def reconnect_connection(self, timeout_seconds):
            self.reconnect_timeout = timeout_seconds
            return type("ConnectionResult", (), {"connected": True, "message": "reconnected"})()

        def get_system_diagnostics(self):
            return {"schema_version": "1.0", "source": "LINUX_HOST"}

    manager = RecoverableConnectionManager()
    clock = {"value": 100.0}
    monkeypatch.setattr(qml_bridge.time, "monotonic", lambda: clock["value"])
    bridge = qml_bridge.QmlOperatorBridge(connection_manager=manager)
    try:
        bridge._schedule_reconnect("SSH-туннель неожиданно завершился.")
        assert manager.loss_handled is True
        assert bridge._reconnect_pending is True
        assert bridge.reconnecting is True
        assert bridge._reconnect_attempt == 0
        assert bridge._next_reconnect_at == 105.0
        assert bridge.model.stale is True
        assert bridge.connectionState == "СВЯЗЬ НАРУШЕНА · ДАННЫЕ УСТАРЕЛИ"

        scheduled = []
        bridge._start_worker = lambda profile_id, operation, is_reconnect=False: scheduled.append(
            (profile_id, operation, is_reconnect)
        )
        clock["value"] = 104.9
        bridge.poll()
        assert scheduled == []
        clock["value"] = 105.0
        bridge.poll()
        assert len(scheduled) == 1
        assert scheduled[0][2] is True
        snapshot, message = scheduled[0][1]()
        assert snapshot["source"] == "LINUX_HOST"
        assert "восстановлена" in message
        assert manager.reconnect_timeout == 5.0

        bridge.disconnectFromStation()
        assert bridge.reconnecting is False
    finally:
        bridge.shutdown()



def test_qml_bridge_uses_bounded_reconnect_backoff(tmp_path: Path, monkeypatch):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    clock = {"value": 100.0}
    monkeypatch.setattr(qml_bridge.time, "monotonic", lambda: clock["value"])
    bridge = qml_bridge.QmlOperatorBridge(connection_manager=InertConnectionManager())
    try:
        bridge._reconnect_pending = True
        expected_delays = (5.0, 10.0, 20.0, 30.0, 30.0)
        for attempt, delay in enumerate(expected_delays):
            clock["value"] += 100.0
            bridge._on_operation_finished(False, "temporary Gateway failure", None)
            assert bridge._reconnect_attempt == attempt + 1
            assert bridge._next_reconnect_at == clock["value"] + delay
            assert bridge.reconnecting is True
    finally:
        bridge.shutdown()



def test_saving_profile_while_disconnected_updates_automatic_reconnect_target(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    class DisconnectedConnectionManager(InertConnectionManager):
        def __init__(self):
            self.profile = ConnectionProfile(
                host="192.0.2.10", username="operator"
            )

    manager = DisconnectedConnectionManager()
    bridge = qml_bridge.QmlOperatorBridge(connection_manager=manager)
    try:
        bridge._reconnect_pending = True
        bridge._reconnect_attempt = 2
        bridge._next_reconnect_at = 123.0
        bridge.setHost("192.0.2.20")
        bridge.setUsername("operator")
        assert bridge.saveProfile() is True

        assert bridge.reconnecting is True
        profile_key = bridge._session_key(bridge.selectedProfileId)
        assert bridge._reconnect_pending_by_profile[profile_key] is True
        assert bridge._reconnect_attempt_by_profile[profile_key] == 2
        assert bridge._next_reconnect_by_profile[profile_key] == 123.0
        assert manager.profile == bridge._profile
        assert manager.profile.host == "192.0.2.20"
    finally:
        bridge.shutdown()



def test_switching_profile_keeps_other_station_reconnect_pending(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    bridge = qml_bridge.QmlOperatorBridge(connection_manager=InertConnectionManager())
    try:
        bridge.addStationProfile("Станция A")
        first_id = bridge.selectedProfileId
        bridge.addStationProfile("Станция B")
        second_id = bridge.selectedProfileId
        assert first_id and second_id and first_id != second_id

        bridge.selectStationProfile(first_id)
        first_manager = bridge.connection_manager
        first_manager.profile = ConnectionProfile(host="192.0.2.10", username="operator")
        bridge._schedule_reconnect("Проверка восстановления первой станции.", first_id)
        assert bridge.reconnecting is True

        bridge.selectStationProfile(second_id)

        assert bridge.selectedProfileId == second_id
        assert bridge.reconnecting is False
        assert bridge._reconnect_pending_by_profile[first_id] is True
        bridge.selectStationProfile(first_id)
        assert bridge.reconnecting is True
    finally:
        bridge.shutdown()



def test_switching_profiles_preserves_each_station_connection_and_diagnostic_context(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    class SessionManager(InertConnectionManager):
        def __init__(self):
            super().__init__()
            self.is_connected = False

    bridge = qml_bridge.QmlOperatorBridge(
        connection_manager=SessionManager(),
        connection_manager_factory=SessionManager,
    )
    try:
        bridge.addStationProfile("Дом")
        home_id = bridge.selectedProfileId
        bridge.setHost("192.0.2.10")
        bridge.setUsername("operator")
        assert bridge.saveProfile()
        home_manager = bridge.connection_manager
        home_manager.is_connected = True
        bridge._models[home_id].apply_diagnostics({"source": "LINUX_HOST", "station_marker": "home"})
        bridge._station_status[home_id] = "CONNECTED"

        bridge.addStationProfile("Склад")
        warehouse_id = bridge.selectedProfileId
        bridge.setHost("192.0.2.11")
        bridge.setUsername("operator")
        assert bridge.saveProfile()
        warehouse_manager = bridge.connection_manager
        warehouse_manager.is_connected = True
        bridge._models[warehouse_id].apply_diagnostics({"source": "LINUX_HOST", "station_marker": "warehouse"})
        bridge._station_status[warehouse_id] = "CONNECTED"

        bridge.selectStationProfile(home_id)
        assert bridge.connection_manager is home_manager
        assert bridge.connection_manager.is_connected is True
        assert bridge.model.snapshot["station_marker"] == "home"

        bridge.selectStationProfile(warehouse_id)
        assert bridge.connection_manager is warehouse_manager
        assert bridge.connection_manager.is_connected is True
        assert bridge.model.snapshot["station_marker"] == "warehouse"

        statuses = {item["profile_id"]: item["status"] for item in bridge.stationProfiles}
        assert statuses == {home_id: "CONNECTED", warehouse_id: "CONNECTED"}
        # The active log view is scoped to the selected station. An event
        # recorded for another profile must not leak into this view.
        bridge._append_event("Лог дома", home_id)
        assert "[Дом] Лог дома" not in bridge.eventsText
        assert "[Склад] Лог склада" not in bridge.eventsText

        bridge.selectStationProfile(home_id)
        assert "[Дом] Лог дома" in bridge.eventsText
        assert "[Склад] Лог склада" not in bridge.eventsText

        bridge.selectStationProfile(warehouse_id)
        assert "[Дом] Лог дома" not in bridge.eventsText
        bridge._append_event("Лог склада", warehouse_id)
        assert "[Склад] Лог склада" in bridge.eventsText
        assert "[Дом] Лог дома" not in bridge.eventsText

        bridge.selectStationProfile(home_id)
        assert "[Дом] Лог дома" in bridge.eventsText
        assert "[Склад] Лог склада" not in bridge.eventsText
        bridge.selectStationProfile(warehouse_id)
        assert "[Склад] Лог склада" in bridge.eventsText
        assert "[Дом] Лог дома" not in bridge.eventsText
    finally:
        bridge.shutdown()


def test_station_list_collapses_after_three_profiles_and_can_be_reordered(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    bridge = qml_bridge.QmlOperatorBridge(connection_manager=InertConnectionManager())
    try:
        for alias in ("Дом", "Склад", "Антенна", "Мастерская"):
            bridge.addStationProfile(alias)
        assert len(bridge.stationProfiles) == 4
        assert len(bridge.visibleStationProfiles) == 3
        assert bridge.hasMoreStationProfiles is True
        assert bridge.stationProfilesExpanded is False

        first = bridge.stationProfiles[0]["profile_id"]
        fourth = bridge.stationProfiles[3]["profile_id"]
        bridge.moveStationProfile(fourth, -1)
        assert bridge.stationProfiles[2]["profile_id"] == fourth

        bridge.toggleStationProfilesExpanded()
        assert len(bridge.visibleStationProfiles) == 4
        assert bridge.stationProfilesExpanded is True
        bridge.toggleStationProfilesExpanded()
        assert len(bridge.visibleStationProfiles) == 3
        assert bridge.stationProfiles[0]["profile_id"] == first
    finally:
        bridge.shutdown()




def test_qml_profile_sidebar_has_compact_drag_handles_and_overlay_scrolling(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    bridge = qml_bridge.QmlOperatorBridge(connection_manager=InertConnectionManager())
    for alias in (
        "Дом", "Склад", "Антенна", "Мастерская",
        "Резерв", "Сервисная", "Запасная", "Испытательная",
    ):
        bridge.addStationProfile(alias)

    app = QGuiApplication.instance() or QGuiApplication([])
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty("bridge", bridge)
    qml_path = Path(dockstation_operator.__file__).parent / "ui" / "qml" / "Main.qml"
    engine.load(QUrl.fromLocalFile(str(qml_path)))

    try:
        app.processEvents()
        roots = engine.rootObjects()
        assert roots
        root = roots[0]
        profile_list = root.findChild(QObject, "stations.local-profile-list")
        expand_button = root.findChild(QObject, "stations.expand-list-button")
        vertical_scrollbar = root.findChild(QObject, "stations.profile-vertical-scrollbar")
        horizontal_scrollbar = root.findChild(QObject, "stations.profile-horizontal-scrollbar")
        assert profile_list is not None
        assert expand_button is not None and expand_button.property("visible") is True
        assert vertical_scrollbar is not None
        assert horizontal_scrollbar is not None
        assert profile_list.property("count") == 3
        collapsed_width = profile_list.property("width")
        qml_source = qml_path.read_text(encoding="utf-8")
        # ListView delegates are created by Qt's item-view machinery and are not
        # guaranteed to be QObject descendants discoverable from the Window.
        assert 'objectName: "stations.profile-drag-handle."' in qml_source
        assert "cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor" in qml_source
        assert "width: stationProfileList.width - stationProfileList.rightMargin" in qml_source
        assert "rightMargin: 5" in qml_source

        bridge.toggleStationProfilesExpanded()
        app.processEvents()
        assert profile_list.property("count") == 8
        assert profile_list.property("width") == collapsed_width
        assert profile_list.property("contentHeight") > profile_list.property("height")
        assert vertical_scrollbar.property("visible") is True
        assert horizontal_scrollbar.property("visible") is False
        assert profile_list.property("width") == collapsed_width
        assert "stationProfileDisplayModel.move(stationProfileDragIndex, targetIndex, 1)" in qml_source
        assert "stationProfileDragPointerY" in qml_source
    finally:
        bridge.shutdown()
        for root_object in engine.rootObjects():
            root_object.close()
        engine.deleteLater()
        app.processEvents()


def test_qml_bridge_reorders_visible_profiles_and_preserves_hidden_profiles(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    bridge = qml_bridge.QmlOperatorBridge(connection_manager=InertConnectionManager())
    try:
        for alias in ("Дом", "Склад", "Антенна", "Мастерская", "Резерв"):
            bridge.addStationProfile(alias)

        original = [item["profile_id"] for item in bridge.stationProfiles]
        selected_id = bridge.selectedProfileId
        collapsed_order = [original[2], original[0], original[1]]

        assert bridge.stationProfilesExpanded is False
        assert bridge.reorderStationProfiles(collapsed_order) is True
        assert [item["profile_id"] for item in bridge.stationProfiles] == [
            original[2], original[0], original[1], original[3], original[4]
        ]
        assert bridge.selectedProfileId == selected_id
        assert [item.profile_id for item in station_registry.load_registry().profiles] == [
            original[2], original[0], original[1], original[3], original[4]
        ]

        bridge.toggleStationProfilesExpanded()
        expanded_order = [original[4], original[3], original[2], original[0], original[1]]
        assert bridge.reorderStationProfiles(expanded_order) is True
        assert [item["profile_id"] for item in bridge.stationProfiles] == expanded_order
        assert bridge.selectedProfileId == selected_id

        before_invalid_change = [item["profile_id"] for item in bridge.stationProfiles]
        assert bridge.reorderStationProfiles([original[4], original[4]]) is False
        assert [item["profile_id"] for item in bridge.stationProfiles] == before_invalid_change
    finally:
        bridge.shutdown()



def test_qml_logs_follow_selected_station_and_show_last_diagnostics_update(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    bridge = qml_bridge.QmlOperatorBridge(connection_manager=InertConnectionManager())
    bridge.addStationProfile("Дом")
    home_id = bridge.selectedProfileId
    bridge._append_event("Событие дома", home_id)

    bridge.addStationProfile("Склад")
    warehouse_id = bridge.selectedProfileId
    bridge._on_operation_finished(
        warehouse_id,
        True,
        "Связь с Linux Gateway восстановлена; получен новый диагностический снимок.",
        {"source": "LINUX_HOST"},
        True,
        False,
    )

    app = QGuiApplication.instance() or QGuiApplication([])
    engine = QQmlApplicationEngine()
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

        log_text = root.findChild(QObject, "logs.text-area")
        station_context = root.findChild(QObject, "logs.station-context")
        last_update = root.findChild(QObject, "logs.last-update")
        assert log_text is not None and station_context is not None and last_update is not None
        assert "[Склад] Связь с Linux Gateway восстановлена" in log_text.property("text")
        assert "[Дом] Событие дома" not in log_text.property("text")
        assert station_context.property("text") == "Станция: Склад"
        assert "Получено:" in last_update.property("text")
        assert "ДАННЫЕ АКТУАЛЬНЫ" in last_update.property("text")

        bridge.selectStationProfile(home_id)
        app.processEvents()
        assert "[Дом] Событие дома" in log_text.property("text")
        assert "[Склад] Связь с Linux Gateway восстановлена" not in log_text.property("text")
        assert station_context.property("text") == "Станция: Дом"
        assert "Последнее обновление: —" in last_update.property("text")
    finally:
        bridge.shutdown()
        for root_object in engine.rootObjects():
            root_object.close()
        engine.deleteLater()
        app.processEvents()


def test_connecting_first_unsaved_profile_uses_its_new_session_manager(
    tmp_path: Path, monkeypatch
):
    registry_file = tmp_path / "stations.json"
    monkeypatch.setattr(station_registry, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "station_registry_path", lambda: registry_file)
    monkeypatch.setattr(qml_bridge, "load_profile", lambda: ConnectionProfile())

    class FakeManager(InertConnectionManager):
        def connect_to_station(self, profile):
            self.profile = profile
            self.is_connected = True
            return type("ConnectionResult", (), {"connected": True, "message": "connected"})()

        def get_system_diagnostics(self):
            return {"source": "LINUX_HOST", "station_marker": self.profile.host}

    manager = FakeManager()
    bridge = qml_bridge.QmlOperatorBridge(connection_manager=manager)
    started = []
    try:
        bridge.setStationAlias("Новая")
        bridge.setHost("192.0.2.40")
        bridge.setUsername("operator")
        bridge._profile = bridge._replace_profile(
            private_key_path="C:/key", known_hosts_path="C:/known_hosts"
        )
        monkeypatch.setattr(qml_bridge, "validate_profile", lambda profile, require_files: [])
        bridge._start_worker = lambda profile_id, operation, is_reconnect=False: started.append(
            (profile_id, operation)
        )
        bridge.connectToStation()
        assert bridge.selectedProfileId
        assert started[0][0] == bridge.selectedProfileId
        snapshot, _ = started[0][1]()
        assert snapshot["station_marker"] == "192.0.2.40"
    finally:
        bridge.shutdown()
