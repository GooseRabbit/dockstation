"""Connection-profile validation and non-secret local settings storage."""

from __future__ import annotations

import ipaddress
import json
import os
import re
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


_HOST_LABEL = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")
_USERNAME = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$")
_STRING_FIELDS = {"host", "username", "private_key_path", "known_hosts_path"}
_INTEGER_FIELDS = {"ssh_port", "local_port", "remote_api_port"}


@dataclass(frozen=True)
class ConnectionProfile:
    host: str = ""
    username: str = ""
    ssh_port: int = 22
    private_key_path: str = ""
    known_hosts_path: str = ""
    local_port: int = 18080
    remote_api_port: int = 8080


def validate_host(host: object) -> str | None:
    if not isinstance(host, str):
        return "Укажи IP-адрес или имя Linux-виртуальной машины."
    value = host.strip()
    if not value or len(value) > 253 or any(ch.isspace() for ch in value):
        return "Укажи IP-адрес или имя Linux-виртуальной машины без пробелов."
    try:
        parsed = ipaddress.ip_address(value)
        if getattr(parsed, "scope_id", None):
            return "IPv6-адрес с зоной интерфейса пока не поддерживается."
        return None
    except ValueError:
        pass
    normalized = value[:-1] if value.endswith(".") else value
    if not normalized or any(not _HOST_LABEL.fullmatch(label) for label in normalized.split(".")):
        return "Имя узла содержит недопустимые символы."
    return None


def validate_profile(profile: ConnectionProfile, require_files: bool = True) -> list[str]:
    if not isinstance(profile, ConnectionProfile):
        return ["Профиль подключения имеет неверный формат."]

    errors: list[str] = []
    host_error = validate_host(profile.host)
    if host_error:
        errors.append(host_error)

    if not isinstance(profile.username, str) or not _USERNAME.fullmatch(profile.username):
        errors.append("Имя Linux-пользователя должно содержать только буквы, цифры, точку, дефис или подчёркивание.")

    for name, value, minimum in (
        ("SSH-порт", profile.ssh_port, 1),
        ("Локальный порт туннеля", profile.local_port, 1024),
        ("Порт API в Linux", profile.remote_api_port, 1),
    ):
        if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= 65535:
            errors.append(f"{name}: допустимое значение от {minimum} до 65535.")

    for label, value in (("путь к приватному SSH-ключу", profile.private_key_path), ("путь к known_hosts", profile.known_hosts_path)):
        if not isinstance(value, str):
            errors.append(f"Некорректный {label}.")

    if require_files:
        try:
            key_path = Path(profile.private_key_path).expanduser() if isinstance(profile.private_key_path, str) and profile.private_key_path.strip() else None
            if key_path is None or not key_path.is_file():
                errors.append("Файл приватного SSH-ключа не найден.")
        except (OSError, ValueError):
            errors.append("Не удалось проверить файл приватного SSH-ключа.")
        try:
            known_path = Path(profile.known_hosts_path).expanduser() if isinstance(profile.known_hosts_path, str) and profile.known_hosts_path.strip() else None
            if known_path is None or not known_path.is_file():
                errors.append("Файл known_hosts не найден. Сначала добавь проверенный ключ узла Linux.")
            elif known_path.stat().st_size == 0:
                errors.append("Файл known_hosts пуст. Подключение не будет принимать неизвестный ключ автоматически.")
        except (OSError, ValueError):
            errors.append("Не удалось проверить файл known_hosts.")
    return errors


def settings_path() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA")
    if base:
        root = Path(base)
    else:
        root = Path.home() / ".config"
    return root / "DockStationOperator" / "settings.json"


def save_profile(profile: ConnectionProfile, path: Path | None = None) -> Path:
    errors = validate_profile(profile, require_files=False)
    if errors:
        raise ValueError("\n".join(errors))

    target = path or settings_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(asdict(profile), ensure_ascii=False, indent=2)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=f"{target.name}.",
            suffix=".tmp",
            dir=target.parent,
            delete=False,
        ) as temporary:
            temporary_path = Path(temporary.name)
            temporary.write(payload)
            temporary.flush()
            os.fsync(temporary.fileno())
        if os.name == "posix":
            temporary_path.chmod(0o600)
        os.replace(temporary_path, target)
        if os.name == "posix":
            target.chmod(0o600)
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink(missing_ok=True)
            except OSError:
                pass
    return target


def load_profile(path: Path | None = None) -> ConnectionProfile:
    target = path or settings_path()
    try:
        payload: Any = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, UnicodeError):
        return ConnectionProfile()
    if not isinstance(payload, dict):
        return ConnectionProfile()

    # Ignore malformed or unknown values instead of allowing a damaged settings
    # file to crash the UI or change the meaning/type of a connection option.
    filtered: dict[str, object] = {}
    for key in _STRING_FIELDS:
        value = payload.get(key)
        if isinstance(value, str):
            filtered[key] = value
    for key in _INTEGER_FIELDS:
        value = payload.get(key)
        if isinstance(value, int) and not isinstance(value, bool):
            filtered[key] = value
    try:
        return ConnectionProfile(**filtered)
    except TypeError:
        return ConnectionProfile()
