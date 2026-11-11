"""Strict, read-only JSON client for the DockStation Gateway API."""

from __future__ import annotations

import json
import math
import urllib.error
import urllib.request
from datetime import datetime, timedelta
from urllib.parse import urlsplit
from typing import Any


MAX_RESPONSE_BYTES = 2 * 1024 * 1024
_ALLOWED_STATUSES = {"PASS", "NOT_CONFIGURED", "UNAVAILABLE", "UNKNOWN", "WARN", "FAIL"}


class ApiClientError(RuntimeError):
    def __init__(self, message: str, status_code: int | None = None):
        super().__init__(message)
        self.status_code = status_code


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # A redirect could move the request outside the authenticated SSH tunnel.
        return None


def _require_object(value: object, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ApiClientError(f"Поле {field} должно быть JSON-объектом.")
    return value


def _require_string(value: object, field: str, *, nullable: bool = False) -> None:
    if nullable and value is None:
        return
    if not isinstance(value, str):
        raise ApiClientError(f"Поле {field} должно быть строкой" + (" или null." if nullable else "."))


def _require_status(value: object, field: str) -> None:
    if not isinstance(value, str) or value not in _ALLOWED_STATUSES:
        raise ApiClientError(f"Поле {field} содержит неизвестный диагностический статус.")


def _require_string_list(value: object, field: str) -> None:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ApiClientError(f"Поле {field} должно быть списком строк.")


def _validate_status_message(section: dict[str, Any], field: str) -> None:
    _require_status(section.get("status"), f"{field}.status")
    _require_string(section.get("message"), f"{field}.message")


class StationApiClient:
    def __init__(self, base_url: str, timeout_seconds: float = 3.0):
        try:
            parsed = urlsplit(base_url)
            port = parsed.port
        except (TypeError, ValueError) as exc:
            raise ValueError("Адрес Gateway имеет неверный формат.") from exc
        if (
            parsed.scheme != "http"
            or parsed.hostname != "127.0.0.1"
            or parsed.username
            or parsed.password
            or parsed.path not in {"", "/"}
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError("Gateway API разрешён только по HTTP на локальном 127.0.0.1 внутри SSH-туннеля.")
        if port is None or not 1 <= port <= 65535:
            raise ValueError("Порт Gateway API должен быть в диапазоне от 1 до 65535.")
        if isinstance(timeout_seconds, bool) or not isinstance(timeout_seconds, (int, float)) or not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
            raise ValueError("Тайм-аут API должен быть положительным конечным числом.")

        self.base_url = f"http://127.0.0.1:{port}"
        self.timeout_seconds = float(timeout_seconds)
        self._opener = urllib.request.build_opener(_NoRedirectHandler())

    def _get_json(self, route: str) -> dict[str, Any]:
        request = urllib.request.Request(
            self.base_url + route,
            headers={"Accept": "application/json", "Cache-Control": "no-cache"},
            method="GET",
        )
        try:
            with self._opener.open(request, timeout=self.timeout_seconds) as response:
                content_type = response.headers.get_content_type()
                if content_type != "application/json":
                    raise ApiClientError(f"API вернул неожиданный Content-Type: {content_type}.")
                raw = response.read(MAX_RESPONSE_BYTES + 1)
                if len(raw) > MAX_RESPONSE_BYTES:
                    raise ApiClientError("Ответ API превышает допустимый размер 2 МиБ.")
        except urllib.error.HTTPError as exc:
            raise ApiClientError(f"HTTP {exc.code}: сервер отклонил запрос.", exc.code) from exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise ApiClientError(f"Нет ответа от Linux API: {exc}") from exc
        try:
            payload = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ApiClientError("API вернул некорректный JSON.") from exc
        if not isinstance(payload, dict):
            raise ApiClientError("Корневой элемент JSON должен быть объектом.")
        return payload

    def get_health(self) -> dict[str, Any]:
        payload = self._get_json("/api/v1/health")
        self.validate_health_response(payload)
        return payload

    @staticmethod
    def validate_health_response(payload: dict[str, Any]) -> None:
        if payload.get("api_version") != "1" or payload.get("service") != "dockstation-gateway":
            raise ApiClientError("Неизвестный сервис или версия API.")
        if payload.get("status") != "PASS" or payload.get("read_only") is not True:
            raise ApiClientError("Gateway не подтвердил готовность read-only API.")

    def get_system_diagnostics(self) -> dict[str, Any]:
        payload = self._get_json("/api/v1/system/diagnostics")
        self.validate_diagnostics_response(payload)
        return payload

    @staticmethod
    def validate_diagnostics_response(payload: dict[str, Any]) -> None:
        required = {"schema_version", "source", "collected_at_utc", "summary", "platform", "network", "scope"}
        missing = sorted(required.difference(payload))
        if missing:
            raise ApiClientError("В ответе диагностики отсутствуют поля: " + ", ".join(missing))
        if payload.get("schema_version") != "1.0" or payload.get("source") != "LINUX_HOST":
            raise ApiClientError("Неизвестная версия или источник диагностического контракта.")

        collected_at = payload.get("collected_at_utc")
        _require_string(collected_at, "collected_at_utc")
        try:
            parsed_timestamp = datetime.fromisoformat(collected_at.replace("Z", "+00:00"))
        except ValueError as exc:
            raise ApiClientError("Поле collected_at_utc не содержит дату ISO 8601.") from exc
        if parsed_timestamp.tzinfo is None:
            raise ApiClientError("Поле collected_at_utc должно содержать часовой пояс.")
        if parsed_timestamp.utcoffset() != timedelta(0):
            raise ApiClientError("Поле collected_at_utc должно быть указано в UTC.")

        summary = _require_object(payload.get("summary"), "summary")
        _validate_status_message(summary, "summary")
        summary_status = summary.get("status")
        if summary_status not in {"PASS", "WARN", "FAIL"}:
            raise ApiClientError("Поле summary.status должно быть PASS, WARN или FAIL.")
        exit_code = summary.get("exit_code")
        expected_exit_code = {"PASS": 0, "WARN": 1, "FAIL": 2}[summary_status]
        if isinstance(exit_code, bool) or not isinstance(exit_code, int) or exit_code != expected_exit_code:
            raise ApiClientError("Поле summary.exit_code не соответствует итоговому статусу.")

        platform = _require_object(payload.get("platform"), "platform")
        os_section = _require_object(platform.get("os"), "platform.os")
        _validate_status_message(os_section, "platform.os")
        for key in ("name", "version", "identifier"):
            _require_string(os_section.get(key), f"platform.os.{key}", nullable=True)
        _require_string(platform.get("kernel"), "platform.kernel", nullable=True)
        _require_string(platform.get("architecture"), "platform.architecture", nullable=True)
        word_size = platform.get("word_size_bits")
        if isinstance(word_size, bool) or not isinstance(word_size, int) or word_size <= 0:
            raise ApiClientError("Поле platform.word_size_bits должно быть положительным целым числом.")
        uptime = _require_object(platform.get("uptime"), "platform.uptime")
        _validate_status_message(uptime, "platform.uptime")
        seconds = uptime.get("seconds")
        if seconds is not None and (
            isinstance(seconds, bool)
            or not isinstance(seconds, (int, float))
            or not math.isfinite(seconds)
            or seconds < 0
        ):
            raise ApiClientError("Поле platform.uptime.seconds должно быть неотрицательным числом или null.")

        network = _require_object(payload.get("network"), "network")
        interfaces = _require_object(network.get("interfaces"), "network.interfaces")
        _validate_status_message(interfaces, "network.interfaces")
        interface_items = interfaces.get("interfaces")
        if not isinstance(interface_items, list):
            raise ApiClientError("Поле network.interfaces.interfaces должно быть списком.")
        for index, item in enumerate(interface_items):
            label = f"network.interfaces.interfaces[{index}]"
            interface = _require_object(item, label)
            for key in ("name", "kind", "message"):
                _require_string(interface.get(key), f"{label}.{key}")
            _require_string(interface.get("oper_state"), f"{label}.oper_state", nullable=True)
            _require_status(interface.get("status"), f"{label}.status")
            carrier = interface.get("carrier")
            if carrier is not None and not isinstance(carrier, bool):
                raise ApiClientError(f"Поле {label}.carrier должно быть boolean или null.")
            addresses = interface.get("addresses")
            if not isinstance(addresses, list):
                raise ApiClientError(f"Поле {label}.addresses должно быть списком.")
            for address_index, address_item in enumerate(addresses):
                address_label = f"{label}.addresses[{address_index}]"
                address = _require_object(address_item, address_label)
                _require_string(address.get("family"), f"{address_label}.family")
                _require_string(address.get("address"), f"{address_label}.address")
                _require_string(address.get("scope"), f"{address_label}.scope", nullable=True)

        routes = _require_object(network.get("default_routes"), "network.default_routes")
        _validate_status_message(routes, "network.default_routes")
        route_items = routes.get("routes")
        if not isinstance(route_items, list):
            raise ApiClientError("Поле network.default_routes.routes должно быть списком.")
        family_status = _require_object(routes.get("family_status"), "network.default_routes.family_status")
        for family in ("IPv4", "IPv6"):
            _require_status(family_status.get(family), f"network.default_routes.family_status.{family}")
        for index, item in enumerate(route_items):
            label = f"network.default_routes.routes[{index}]"
            route = _require_object(item, label)
            _require_string(route.get("family"), f"{label}.family")
            _require_string(route.get("destination"), f"{label}.destination")
            for key in ("gateway", "interface", "protocol"):
                _require_string(route.get(key), f"{label}.{key}", nullable=True)
            metric = route.get("metric")
            if metric is not None and (isinstance(metric, bool) or not isinstance(metric, int) or metric < 0):
                raise ApiClientError(f"Поле {label}.metric должно быть неотрицательным целым числом или null.")

        dns = _require_object(network.get("dns"), "network.dns")
        _validate_status_message(dns, "network.dns")
        _require_string(dns.get("hostname"), "network.dns.hostname", nullable=True)
        _require_string_list(dns.get("addresses"), "network.dns.addresses")

        ssh = _require_object(network.get("ssh"), "network.ssh")
        _validate_status_message(ssh, "network.ssh")
        units = ssh.get("units")
        if not isinstance(units, list):
            raise ApiClientError("Поле network.ssh.units должно быть списком.")
        for index, item in enumerate(units):
            label = f"network.ssh.units[{index}]"
            unit = _require_object(item, label)
            for key in ("name", "load_state", "active_state", "sub_state", "unit_file_state"):
                _require_string(unit.get(key), f"{label}.{key}")
        for optional_list in ("active_units", "unknown_units", "masked_units"):
            if optional_list in ssh:
                _require_string_list(ssh[optional_list], f"network.ssh.{optional_list}")

        scope = _require_object(payload.get("scope"), "scope")
        if scope.get("read_only") is not True:
            raise ApiClientError("Gateway не подтвердил read-only границу данных.")
        for key in ("hardware_controller_connected", "physical_sensors_configured"):
            if not isinstance(scope.get(key), bool):
                raise ApiClientError(f"Поле scope.{key} должно быть логическим.")
        _require_string(scope.get("note"), "scope.note")

    @staticmethod
    def map_api_error(error: Exception) -> str:
        if isinstance(error, ApiClientError):
            return str(error)
        return "Не удалось получить диагностические данные. Проверь подключение и журнал событий."
