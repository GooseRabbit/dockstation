"""Strict-host-key OpenSSH local tunnel lifecycle."""

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import time
from pathlib import Path
from typing import Callable

from ..config.settings import ConnectionProfile, validate_profile


class SshTunnelError(RuntimeError):
    pass


PopenFactory = Callable[..., subprocess.Popen]


def find_ssh_executable() -> str:
    candidates = (shutil.which("ssh.exe"), shutil.which("ssh"))
    for candidate in candidates:
        if candidate:
            return candidate
    windows_dir = os.environ.get("WINDIR")
    if windows_dir:
        bundled = Path(windows_dir) / "System32" / "OpenSSH" / "ssh.exe"
        if bundled.is_file():
            return str(bundled)
    raise SshTunnelError("Не найден OpenSSH ssh.exe. Установи компонент OpenSSH Client в Windows.")


def find_ssh_keygen_executable(ssh_executable: str | None = None) -> str:
    if ssh_executable:
        sibling = Path(ssh_executable).expanduser().with_name("ssh-keygen.exe" if os.name == "nt" else "ssh-keygen")
        if sibling.is_file():
            return str(sibling)
    candidates = (shutil.which("ssh-keygen.exe"), shutil.which("ssh-keygen"))
    for candidate in candidates:
        if candidate:
            return candidate
    windows_dir = os.environ.get("WINDIR")
    if windows_dir:
        bundled = Path(windows_dir) / "System32" / "OpenSSH" / "ssh-keygen.exe"
        if bundled.is_file():
            return str(bundled)
    raise SshTunnelError("Не найден ssh-keygen.exe; проверить доверенный host key невозможно.")


def known_hosts_label(profile: ConnectionProfile) -> str:
    host = profile.host.strip()
    if profile.ssh_port != 22:
        return f"[{host}]:{profile.ssh_port}"
    return host


def verify_known_host(profile: ConnectionProfile, ssh_keygen_executable: str | None = None) -> bool:
    """Require a matching entry in the selected known_hosts file."""
    try:
        known_hosts = Path(profile.known_hosts_path).expanduser()
        if not known_hosts.is_file() or known_hosts.stat().st_size == 0:
            return False
    except (OSError, ValueError):
        return False
    executable = ssh_keygen_executable or find_ssh_keygen_executable()
    try:
        result = subprocess.run(
            [executable, "-F", known_hosts_label(profile), "-f", str(known_hosts)],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=3.0,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SshTunnelError(f"Не удалось проверить known_hosts: {exc}") from exc
    return result.returncode == 0 and bool(result.stdout.strip())


def build_ssh_command(executable: str, profile: ConnectionProfile) -> list[str]:
    errors = validate_profile(profile, require_files=True)
    if errors:
        raise SshTunnelError("\n".join(errors))
    host = profile.host.strip()
    forward = f"127.0.0.1:{profile.local_port}:127.0.0.1:{profile.remote_api_port}"
    options = [
        "BatchMode=yes",
        "StrictHostKeyChecking=yes",
        "VerifyHostKeyDNS=no",
        "CheckHostIP=no",
        f'UserKnownHostsFile="{Path(profile.known_hosts_path).expanduser().as_posix()}"',
        "UpdateHostKeys=no",
        "IdentitiesOnly=yes",
        "PubkeyAuthentication=yes",
        "PreferredAuthentications=publickey",
        "PasswordAuthentication=no",
        "KbdInteractiveAuthentication=no",
        "HostbasedAuthentication=no",
        "GSSAPIAuthentication=no",
        "ForwardAgent=no",
        "PermitLocalCommand=no",
        "ControlMaster=no",
        "ExitOnForwardFailure=yes",
        "ConnectTimeout=5",
        "ConnectionAttempts=1",
        "ServerAliveInterval=15",
        "ServerAliveCountMax=3",
        "LogLevel=ERROR",
    ]
    # Ignore user/system ssh_config files: they can silently add proxy hops or
    # unrelated forwards. All connection behavior is explicit below.
    command = [executable, "-F", "none", "-N", "-T", "-p", str(profile.ssh_port)]
    for option in options:
        command.extend(("-o", option))
    command.extend(("-i", str(Path(profile.private_key_path).expanduser()), "-L", forward, f"{profile.username}@{host}"))
    return command


def is_local_port_available(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


class SshTunnel:
    def __init__(self, popen_factory: PopenFactory = subprocess.Popen):
        self._popen_factory = popen_factory
        self._process: subprocess.Popen | None = None

    @property
    def is_running(self) -> bool:
        return self._process is not None and self._process.poll() is None

    def start(self, profile: ConnectionProfile, executable: str | None = None) -> None:
        if self.is_running:
            raise SshTunnelError("SSH-туннель уже запущен.")
        errors = validate_profile(profile, require_files=True)
        if errors:
            raise SshTunnelError("\n".join(errors))
        if not is_local_port_available(profile.local_port):
            raise SshTunnelError(f"Локальный порт {profile.local_port} уже занят.")

        ssh_executable = executable or find_ssh_executable()
        keygen_executable = find_ssh_keygen_executable(ssh_executable)
        if not verify_known_host(profile, keygen_executable):
            raise SshTunnelError(
                f"В known_hosts нет проверенной записи для {known_hosts_label(profile)}. "
                "Добавь ключ Linux VM вручную только после независимой проверки отпечатка."
            )

        options: dict[str, object] = {
            "stdin": subprocess.DEVNULL,
            "stdout": subprocess.DEVNULL,
            "stderr": subprocess.DEVNULL,
            "close_fds": True,
        }
        if os.name == "nt":
            options["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            self._process = self._popen_factory(build_ssh_command(ssh_executable, profile), **options)
        except OSError as exc:
            self._process = None
            raise SshTunnelError(f"Не удалось запустить OpenSSH: {exc}") from exc

    def wait_for_process_or_stop(self, timeout_seconds: float = 10.0) -> bool:
        """Wait until ssh exits or remains alive for the requested interval."""
        if self._process is None or timeout_seconds < 0:
            return False
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            if self._process.poll() is not None:
                return False
            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
        return self._process.poll() is None

    def stop(self) -> None:
        process = self._process
        self._process = None
        if process is None:
            return
        try:
            if process.poll() is None:
                process.terminate()
        except OSError:
            return
        try:
            process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            try:
                process.kill()
                process.wait(timeout=2.0)
            except (OSError, subprocess.TimeoutExpired):
                pass
