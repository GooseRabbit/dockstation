from __future__ import annotations

import json
from pathlib import Path

from dockstation_operator.config.settings import (
    ConnectionProfile,
    load_profile,
    save_profile,
    validate_host,
    validate_profile,
)


def test_validate_ipv4_ipv6_and_dns_hosts():
    assert validate_host("192.0.2.10") is None
    assert validate_host("2001:db8::10") is None
    assert validate_host("dockstation-vm.local") is None


def test_validate_host_rejects_whitespace_and_injection():
    assert validate_host("host name") is not None
    assert validate_host("host;whoami") is not None
    assert validate_host("") is not None


def test_validate_profile_requires_key_and_known_hosts(tmp_path: Path):
    profile = ConnectionProfile(host="192.0.2.10", username="dockstation", private_key_path="", known_hosts_path="")
    errors = validate_profile(profile)
    assert any("приватного SSH-ключа" in error for error in errors)
    assert any("known_hosts" in error for error in errors)


def test_validate_profile_accepts_existing_files(tmp_path: Path):
    key = tmp_path / "id_ed25519"
    hosts = tmp_path / "known_hosts"
    key.write_text("test-private-key-placeholder", encoding="utf-8")
    hosts.write_text("192.0.2.10 ssh-ed25519 AAAATEST", encoding="utf-8")
    profile = ConnectionProfile(
        host="192.0.2.10", username="dockstation", private_key_path=str(key), known_hosts_path=str(hosts)
    )
    assert validate_profile(profile) == []


def test_validate_profile_rejects_invalid_ports():
    profile = ConnectionProfile(host="192.0.2.10", username="dockstation", ssh_port=0)
    errors = validate_profile(profile, require_files=False)
    assert any("SSH-порт" in error for error in errors)


def test_save_and_load_profile_persist_paths_but_no_secret(tmp_path: Path):
    path = tmp_path / "settings.json"
    profile = ConnectionProfile(
        host="dockstation-vm.local", username="operator", ssh_port=2222,
        private_key_path="C:/Users/operator/.ssh/id_ed25519",
        known_hosts_path="C:/Users/operator/.ssh/known_hosts", local_port=18080,
    )
    save_profile(profile, path)
    loaded = load_profile(path)
    assert loaded == profile
    payload = json.loads(path.read_text(encoding="utf-8"))
    assert "password" not in payload
    assert "private_key" not in payload
    assert payload["private_key_path"] == profile.private_key_path


def test_load_profile_ignores_wrongly_typed_values(tmp_path: Path):
    path = tmp_path / "settings.json"
    path.write_text(
        json.dumps({"host": None, "username": "operator", "ssh_port": True, "local_port": "18080", "remote_api_port": 8080}),
        encoding="utf-8",
    )
    loaded = load_profile(path)
    assert loaded.host == ""
    assert loaded.username == "operator"
    assert loaded.ssh_port == 22
    assert loaded.local_port == 18080
    assert loaded.remote_api_port == 8080


def test_validate_host_rejects_ipv6_scoped_address():
    assert validate_host("fe80::1%eth0") is not None


def test_settings_file_is_replaced_atomically_and_private_on_posix(tmp_path: Path):
    path = tmp_path / "settings.json"
    profile = ConnectionProfile(host="repka.local", username="operator")
    save_profile(profile, path)
    assert path.read_text(encoding="utf-8").startswith("{")
    if __import__("os").name == "posix":
        assert path.stat().st_mode & 0o777 == 0o600


def test_validate_profile_handles_null_bytes_in_paths_without_crashing():
    profile = ConnectionProfile(
        host="192.0.2.10", username="operator",
        private_key_path="bad\x00key", known_hosts_path="bad\x00known_hosts",
    )
    errors = validate_profile(profile)
    assert len(errors) >= 2
    assert any("приватного SSH-ключа" in error for error in errors)
    assert any("known_hosts" in error for error in errors)
