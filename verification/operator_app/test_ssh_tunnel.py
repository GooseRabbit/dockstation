from __future__ import annotations

import subprocess
from pathlib import Path
from unittest.mock import patch

import pytest

from dockstation_operator.config.settings import ConnectionProfile
from dockstation_operator.connection.ssh_tunnel import (
    SshTunnel,
    SshTunnelError,
    known_hosts_label,
    build_ssh_command,
    verify_known_host,
)


def make_profile(tmp_path: Path, host: str = "192.0.2.15", port: int = 22) -> ConnectionProfile:
    key = tmp_path / "id_ed25519"
    known = tmp_path / "known_hosts"
    key.write_text("test-private-key-placeholder", encoding="utf-8")
    known.write_text("host ssh-ed25519 AAAATEST", encoding="utf-8")
    return ConnectionProfile(host=host, username="operator", ssh_port=port, private_key_path=str(key), known_hosts_path=str(known))


def test_build_ssh_command_enforces_strict_host_key_and_loopback_forward(tmp_path: Path):
    profile = make_profile(tmp_path)
    command = build_ssh_command("ssh.exe", profile)
    assert command[0] == "ssh.exe"
    assert "StrictHostKeyChecking=yes" in command
    assert command[1:3] == ["-F", "none"]
    assert "BatchMode=yes" in command
    assert "IdentitiesOnly=yes" in command
    assert "PubkeyAuthentication=yes" in command
    assert "PreferredAuthentications=publickey" in command
    assert "PasswordAuthentication=no" in command
    assert "KbdInteractiveAuthentication=no" in command
    assert "HostbasedAuthentication=no" in command
    assert "GSSAPIAuthentication=no" in command
    assert "ExitOnForwardFailure=yes" in command
    assert not any("GlobalKnownHostsFile" in argument for argument in command)
    assert "UpdateHostKeys=no" in command
    assert "ForwardAgent=no" in command
    assert "ClearAllForwardings=yes" not in command
    assert "VerifyHostKeyDNS=no" in command
    assert "ConnectTimeout=5" in command
    assert "accept-new" not in " ".join(command)
    assert command[command.index("-L") + 1] == "127.0.0.1:18080:127.0.0.1:8080"
    assert command[-1] == "operator@192.0.2.15"
    assert "-i" in command and str(Path(profile.private_key_path)) in command


def test_build_ssh_command_does_not_use_shell(tmp_path: Path):
    profile = make_profile(tmp_path, host="host;touch-unsafe")
    with pytest.raises(SshTunnelError):
        build_ssh_command("ssh.exe", profile)


def test_known_hosts_label_for_nonstandard_port_and_ipv6(tmp_path: Path):
    profile = make_profile(tmp_path, host="2001:db8::15", port=2222)
    assert known_hosts_label(profile) == "[2001:db8::15]:2222"


def test_verify_known_host_requires_matching_entry(tmp_path: Path):
    profile = make_profile(tmp_path)
    fake = tmp_path / "ssh-keygen"
    fake.write_text("#!/bin/sh\nprintf '# host found\\nkey\\n'\n", encoding="utf-8")
    fake.chmod(0o755)
    assert verify_known_host(profile, str(fake)) is True


def test_verify_known_host_fails_for_unknown_key(tmp_path: Path):
    profile = make_profile(tmp_path)
    fake = tmp_path / "ssh-keygen"
    fake.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
    fake.chmod(0o755)
    assert verify_known_host(profile, str(fake)) is False


def test_tunnel_start_refuses_unknown_host_key(tmp_path: Path):
    profile = make_profile(tmp_path)
    with (
        patch("dockstation_operator.connection.ssh_tunnel.find_ssh_keygen_executable", return_value="ssh-keygen.exe"),
        patch("dockstation_operator.connection.ssh_tunnel.is_local_port_available", return_value=True),
        patch("dockstation_operator.connection.ssh_tunnel.verify_known_host", return_value=False),
    ):
        tunnel = SshTunnel()
        with pytest.raises(SshTunnelError, match="known_hosts"):
            tunnel.start(profile, executable="ssh.exe")


def test_tunnel_stops_only_its_owned_process(tmp_path: Path):
    profile = make_profile(tmp_path)

    class FakeProcess:
        def __init__(self, *_args, **_kwargs):
            self.terminated = False
            self.killed = False

        def poll(self):
            return None if not self.terminated else 0

        def terminate(self):
            self.terminated = True

        def wait(self, timeout=None):
            return 0

        def kill(self):
            self.killed = True
            self.terminated = True

    process = FakeProcess()
    with (
        patch("dockstation_operator.connection.ssh_tunnel.find_ssh_keygen_executable", return_value="ssh-keygen.exe"),
        patch("dockstation_operator.connection.ssh_tunnel.verify_known_host", return_value=True),
        patch("dockstation_operator.connection.ssh_tunnel.is_local_port_available", return_value=True),
    ):
        tunnel = SshTunnel(popen_factory=lambda *_args, **_kwargs: process)
        tunnel.start(profile, executable="ssh.exe")
        assert tunnel.is_running
        tunnel.stop()
    assert process.terminated is True
    assert process.killed is False
    assert tunnel.is_running is False


def test_known_hosts_path_with_spaces_is_quoted_for_openssh(tmp_path: Path):
    folder = tmp_path / "user profile with spaces"
    folder.mkdir()
    key = folder / "private key"
    known = folder / "known hosts"
    key.write_text("test-private-key-placeholder", encoding="utf-8")
    known.write_text("192.0.2.15 ssh-ed25519 AAAATEST", encoding="utf-8")
    profile = ConnectionProfile(
        host="192.0.2.15", username="operator",
        private_key_path=str(key), known_hosts_path=str(known),
    )
    command = build_ssh_command("ssh.exe", profile)
    option = next(argument for argument in command if argument.startswith("UserKnownHostsFile="))
    assert option == f'UserKnownHostsFile="{known.as_posix()}"'
