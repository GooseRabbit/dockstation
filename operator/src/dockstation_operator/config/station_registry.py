"""Versioned local registry for laboratory station connection profiles.

This registry is deliberately not the product account/OwnerSpace catalogue.
It stores only client-local aliases and SSH connection settings. The server
account catalogue remains the authority for Station ID ownership and access.
"""

from __future__ import annotations

import json
import os
import socket
import tempfile
import uuid
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from typing import Any

from .settings import ConnectionProfile, validate_profile


REGISTRY_SCHEMA_VERSION = 1
MAX_ALIAS_LENGTH = 64


@dataclass(frozen=True)
class StationProfile:
    """One locally saved laboratory connection profile."""

    profile_id: str
    alias: str
    connection: ConnectionProfile


@dataclass(frozen=True)
class StationRegistry:
    """Local profile collection; profile_id is not a Station ID."""

    profiles: tuple[StationProfile, ...] = ()
    selected_profile_id: str | None = None


def station_registry_path() -> Path:
    """Return the client-local registry path without changing legacy settings."""
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA")
    root = Path(base) if base else Path.home() / ".config"
    return root / "DockStationOperator" / "stations.json"


def validate_alias(alias: object) -> str | None:
    if not isinstance(alias, str):
        return "Название станции должно быть текстом."
    value = alias.strip()
    if not value:
        return "Укажи название станции."
    if len(value) > MAX_ALIAS_LENGTH:
        return f"Название станции не должно превышать {MAX_ALIAS_LENGTH} символов."
    if any(ord(character) < 32 for character in value):
        return "Название станции содержит недопустимые управляющие символы."
    return None



def allocate_local_port(registry: StationRegistry, start_port: int = 18080) -> int:
    """Find a loopback port not assigned to another profile and currently bindable."""
    used_ports = {
        item.connection.local_port
        for item in registry.profiles
        if isinstance(item.connection, ConnectionProfile)
    }
    for candidate in range(max(1024, start_port), 65536):
        if candidate in used_ports:
            continue
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.bind(("127.0.0.1", candidate))
        except OSError:
            continue
        else:
            return candidate
        finally:
            probe.close()
    raise ValueError("Не удалось подобрать свободный локальный порт для SSH-туннеля.")


def repair_duplicate_local_ports(registry: StationRegistry) -> StationRegistry:
    """Repair duplicate saved tunnel ports while preserving profile order and selection."""
    used_ports: set[int] = set()
    updated: list[StationProfile] = []
    changed = False
    for item in registry.profiles:
        port = item.connection.local_port
        if port in used_ports:
            partial = StationRegistry(tuple(updated), registry.selected_profile_id)
            port = allocate_local_port(partial)
            item = StationProfile(item.profile_id, item.alias, replace(item.connection, local_port=port))
            changed = True
        used_ports.add(port)
        updated.append(item)
    return StationRegistry(tuple(updated), registry.selected_profile_id) if changed else registry


def validate_registry(registry: StationRegistry) -> list[str]:
    if not isinstance(registry, StationRegistry):
        return ["Реестр станций имеет неверный формат."]
    errors: list[str] = []
    ids: set[str] = set()
    local_ports: set[int] = set()
    for index, item in enumerate(registry.profiles):
        prefix = f"Запись {index + 1}: "
        if not isinstance(item, StationProfile):
            errors.append(prefix + "неверный формат.")
            continue
        if not isinstance(item.profile_id, str) or not item.profile_id.strip():
            errors.append(prefix + "отсутствует локальный идентификатор профиля.")
        elif item.profile_id in ids:
            errors.append(prefix + "повторяется локальный идентификатор профиля.")
        else:
            ids.add(item.profile_id)
        alias_error = validate_alias(item.alias)
        if alias_error:
            errors.append(prefix + alias_error)
        if not isinstance(item.connection, ConnectionProfile):
            errors.append(prefix + "неверный формат параметров подключения.")
        elif not all(
            isinstance(getattr(item.connection, field_name), str)
            for field_name in ("host", "username", "private_key_path", "known_hosts_path")
        ):
            errors.append(prefix + "некорректное текстовое поле подключения.")
        elif not item.connection.host.strip() and not item.connection.username.strip():
            # An empty draft is allowed in the local editor; connect/save validates
            # the complete profile before any network operation.
            for field_name, minimum in (("ssh_port", 1), ("local_port", 1024), ("remote_api_port", 1)):
                value = getattr(item.connection, field_name)
                if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= 65535:
                    errors.append(prefix + "некорректный порт подключения.")
        else:
            errors.extend(prefix + error for error in validate_profile(item.connection, require_files=False))
        if isinstance(item.connection, ConnectionProfile):
            local_port = item.connection.local_port
            if isinstance(local_port, int) and not isinstance(local_port, bool):
                if local_port in local_ports:
                    errors.append(prefix + "локальный порт SSH-туннеля уже используется другим профилем.")
                local_ports.add(local_port)
    if registry.selected_profile_id is not None and (
        not isinstance(registry.selected_profile_id, str)
        or registry.selected_profile_id not in ids
    ):
        errors.append("Выбранный профиль отсутствует в реестре.")
    return errors


def load_registry(path: Path | None = None) -> StationRegistry:
    """Load a registry. Missing, malformed or unsupported files fail closed to empty."""
    target = path or station_registry_path()
    try:
        payload: Any = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, UnicodeError):
        return StationRegistry()
    if not isinstance(payload, dict):
        return StationRegistry()
    schema_version = payload.get("schema_version")
    if isinstance(schema_version, bool) or schema_version != REGISTRY_SCHEMA_VERSION:
        return StationRegistry()
    records = payload.get("profiles")
    selected = payload.get("selected_profile_id")
    if not isinstance(records, list) or (selected is not None and not isinstance(selected, str)):
        return StationRegistry()

    profiles: list[StationProfile] = []
    for record in records:
        if not isinstance(record, dict):
            return StationRegistry()
        connection_data = record.get("connection")
        if not isinstance(connection_data, dict):
            return StationRegistry()
        try:
            connection = ConnectionProfile(**connection_data)
            profiles.append(
                StationProfile(
                    profile_id=record["profile_id"],
                    alias=record["alias"],
                    connection=connection,
                )
            )
        except (KeyError, TypeError):
            return StationRegistry()

    registry = repair_duplicate_local_ports(StationRegistry(tuple(profiles), selected))
    return registry if not validate_registry(registry) else StationRegistry()


def save_registry(registry: StationRegistry, path: Path | None = None) -> Path:
    errors = validate_registry(registry)
    if errors:
        raise ValueError("\n".join(errors))

    target = path or station_registry_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": REGISTRY_SCHEMA_VERSION,
        "selected_profile_id": registry.selected_profile_id,
        "profiles": [
            {
                "profile_id": item.profile_id,
                "alias": item.alias.strip(),
                "connection": asdict(item.connection),
            }
            for item in registry.profiles
        ],
    }
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
            json.dump(payload, temporary, ensure_ascii=False, indent=2)
            temporary.write("\n")
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


def add_profile(
    registry: StationRegistry,
    alias: str,
    connection: ConnectionProfile,
    profile_id: str | None = None,
) -> StationRegistry:
    alias_error = validate_alias(alias)
    if alias_error:
        raise ValueError(alias_error)
    new_id = profile_id or str(uuid.uuid4())
    if any(item.profile_id == new_id for item in registry.profiles):
        raise ValueError("Профиль с таким локальным идентификатором уже существует.")
    result = StationRegistry(
        profiles=registry.profiles + (StationProfile(new_id, alias.strip(), connection),),
        selected_profile_id=new_id,
    )
    errors = validate_registry(result)
    if errors:
        raise ValueError("\n".join(errors))
    return result


def rename_profile(registry: StationRegistry, profile_id: str, alias: str) -> StationRegistry:
    alias_error = validate_alias(alias)
    if alias_error:
        raise ValueError(alias_error)
    if not any(item.profile_id == profile_id for item in registry.profiles):
        raise KeyError("Профиль не найден.")
    result = StationRegistry(
        profiles=tuple(
            StationProfile(item.profile_id, alias.strip(), item.connection)
            if item.profile_id == profile_id else item
            for item in registry.profiles
        ),
        selected_profile_id=registry.selected_profile_id,
    )
    errors = validate_registry(result)
    if errors:
        raise ValueError("\n".join(errors))
    return result


def select_profile(registry: StationRegistry, profile_id: str) -> StationRegistry:
    if not any(item.profile_id == profile_id for item in registry.profiles):
        raise KeyError("Профиль не найден.")
    return StationRegistry(registry.profiles, profile_id)


def move_profile(registry: StationRegistry, profile_id: str, offset: int) -> StationRegistry:
    """Move a profile one position up (-1) or down (+1), preserving selection."""
    if isinstance(offset, bool) or offset not in (-1, 1):
        raise ValueError("Направление перемещения должно быть -1 или 1.")
    items = list(registry.profiles)
    index = next((i for i, item in enumerate(items) if item.profile_id == profile_id), None)
    if index is None:
        raise KeyError("Профиль не найден.")
    target = index + offset
    if target < 0 or target >= len(items):
        return registry
    items[index], items[target] = items[target], items[index]
    return StationRegistry(tuple(items), registry.selected_profile_id)


def remove_profile(registry: StationRegistry, profile_id: str) -> StationRegistry:
    if not any(item.profile_id == profile_id for item in registry.profiles):
        raise KeyError("Профиль не найден.")
    remaining = tuple(item for item in registry.profiles if item.profile_id != profile_id)
    selected = registry.selected_profile_id
    if selected == profile_id:
        selected = remaining[0].profile_id if remaining else None
    return StationRegistry(remaining, selected)
