from __future__ import annotations

from pathlib import Path

from dockstation_operator.config.settings import ConnectionProfile
from dockstation_operator.config import station_registry
from dockstation_operator.ui import qml_bridge


class InertConnectionManager:
    is_connected = False

    def disconnect_from_station(self):
        return None


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
        bridge._start_worker = scheduled.append
        clock["value"] = 104.9
        bridge.poll()
        assert scheduled == []
        clock["value"] = 105.0
        bridge.poll()
        assert len(scheduled) == 1
        snapshot, message = scheduled[0]()
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
