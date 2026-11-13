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
        bridge.saveProfile()

        bridge.addStationProfile("Склад")
        second_id = bridge.selectedProfileId
        assert second_id and second_id != first_id
        assert len(bridge.stationProfiles) == 2
        assert [item["alias"] for item in bridge.stationProfiles] == ["Дом · основная", "Склад"]
        assert all(item["status"] == "UNKNOWN" for item in bridge.stationProfiles)

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
