from __future__ import annotations

import json
from pathlib import Path

import pytest

from dockstation_operator.config.settings import ConnectionProfile
from dockstation_operator.config.station_registry import (
    StationRegistry,
    add_profile,
    load_registry,
    remove_profile,
    rename_profile,
    save_registry,
    select_profile,
    validate_alias,
)


def profile(host: str, port: int) -> ConnectionProfile:
    return ConnectionProfile(
        host=host,
        username="operator",
        ssh_port=22,
        private_key_path="C:/keys/operator",
        known_hosts_path="C:/keys/known_hosts",
        local_port=port,
        remote_api_port=8080,
    )


def test_registry_add_select_rename_and_remove_are_independent(tmp_path: Path):
    registry = StationRegistry()
    registry = add_profile(registry, "Дом", profile("192.0.2.10", 18080), "local-1")
    registry = add_profile(registry, "Склад", profile("192.0.2.11", 18081), "local-2")

    assert registry.selected_profile_id == "local-1"
    assert [item.alias for item in registry.profiles] == ["Дом", "Склад"]

    registry = select_profile(registry, "local-2")
    registry = rename_profile(registry, "local-1", "Основная")
    assert registry.selected_profile_id == "local-2"
    assert registry.profiles[0].alias == "Основная"
    assert registry.profiles[1].alias == "Склад"
    assert registry.profiles[0].connection.host == "192.0.2.10"

    registry = remove_profile(registry, "local-2")
    assert registry.selected_profile_id == "local-1"
    assert [item.profile_id for item in registry.profiles] == ["local-1"]


def test_registry_round_trips_versioned_profiles_without_secret_material(tmp_path: Path):
    path = tmp_path / "stations.json"
    registry = add_profile(StationRegistry(), "Дом", profile("repka-home.local", 18080), "local-1")
    save_registry(registry, path)

    loaded = load_registry(path)
    assert loaded == registry
    raw = json.loads(path.read_text(encoding="utf-8"))
    assert raw["schema_version"] == 1
    assert "private_key_path" in raw["profiles"][0]["connection"]
    assert "private_key" not in raw["profiles"][0]["connection"]


def test_registry_rejects_invalid_alias_and_duplicate_profile_id():
    assert validate_alias("  ") is not None
    assert validate_alias("x" * 65) is not None

    registry = add_profile(StationRegistry(), "Дом", profile("192.0.2.10", 18080), "same")
    with pytest.raises(ValueError, match="идентификатор"):
        add_profile(registry, "Склад", profile("192.0.2.11", 18081), "same")


def test_registry_fails_closed_for_unsupported_or_malformed_data(tmp_path: Path):
    path = tmp_path / "stations.json"
    path.write_text(json.dumps({"schema_version": 999, "profiles": []}), encoding="utf-8")
    assert load_registry(path) == StationRegistry()

    path.write_text("{not-json", encoding="utf-8")
    assert load_registry(path) == StationRegistry()


def test_registry_select_and_remove_unknown_profiles_raise():
    registry = StationRegistry()
    with pytest.raises(KeyError):
        select_profile(registry, "missing")
    with pytest.raises(KeyError):
        rename_profile(registry, "missing", "Дом")
    with pytest.raises(KeyError):
        remove_profile(registry, "missing")
