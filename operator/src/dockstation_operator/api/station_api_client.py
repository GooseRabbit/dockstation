"""Strict JSON client for diagnostics and the explicitly test-only Gateway route."""

from __future__ import annotations

import json
import math
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timedelta
from urllib.parse import urlsplit
from typing import Any


MAX_RESPONSE_BYTES = 2 * 1024 * 1024
DEFAULT_DIAGNOSTICS_TIMEOUT_SECONDS = 15.0
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



_CAPABILITY_STATUSES = {"AVAILABLE", "DETECTED", "NOT_CONFIGURED", "UNAVAILABLE", "UNKNOWN"}
_PLATFORM_PROFILES = {"GENERIC_LINUX", "REPKA_PI5", "UNKNOWN"}
_PLATFORM_INTERFACE_KEYS = ("uart", "gpio", "i2c", "spi", "pwm", "camera")


def _validate_platform_capabilities(value: object) -> None:
    section = _require_object(value, "platform.capabilities")
    profile = section.get("profile")
    if not isinstance(profile, str) or profile not in _PLATFORM_PROFILES:
        raise ApiClientError("Поле platform.capabilities.profile содержит неизвестный профиль.")
    device_tree = _require_object(section.get("device_tree"), "platform.capabilities.device_tree")
    for field in ("model", "compatible"):
        _require_string(device_tree.get(field), f"platform.capabilities.device_tree.{field}", nullable=True)
    for field in ("model_available", "compatible_available", "kernel_cmdline_available"):
        if not isinstance(device_tree.get(field), bool):
            raise ApiClientError(f"Поле platform.capabilities.device_tree.{field} должно быть boolean.")
    if device_tree["model_available"] != bool(device_tree.get("model")):
        raise ApiClientError("Доступность Device Tree model не соответствует значению.")
    if device_tree["compatible_available"] != bool(device_tree.get("compatible")):
        raise ApiClientError("Доступность Device Tree compatible не соответствует значению.")
    interfaces = _require_object(section.get("interfaces"), "platform.capabilities.interfaces")
    max_uint32 = (1 << 32) - 1
    for key in _PLATFORM_INTERFACE_KEYS:
        label = f"platform.capabilities.interfaces.{key}"
        item = _require_object(interfaces.get(key), label)
        status = item.get("status")
        if not isinstance(status, str) or status not in _CAPABILITY_STATUSES:
            raise ApiClientError(f"Поле {label}.status содержит неизвестный статус.")
        counts: dict[str, int] = {}
        for field in ("device_nodes", "accessible_device_nodes", "sysfs_controllers", "kernel_console_conflicts"):
            count = item.get(field)
            if isinstance(count, bool) or not isinstance(count, int) or not 0 <= count <= max_uint32:
                raise ApiClientError(f"Поле {label}.{field} должно быть целым числом в диапазоне uint32.")
            counts[field] = count
        if counts["accessible_device_nodes"] > counts["device_nodes"]:
            raise ApiClientError(f"Поле {label}.accessible_device_nodes превышает число найденных устройств.")
        if counts["kernel_console_conflicts"] > counts["device_nodes"]:
            raise ApiClientError(f"Поле {label}.kernel_console_conflicts превышает число найденных устройств.")
        _require_string(item.get("candidate_device_path"), f"{label}.candidate_device_path", nullable=True)
        if item.get("candidate_only") is not True:
            raise ApiClientError(f"Поле {label}.candidate_only должно быть true: путь не выбирается автоматически.")
        if item.get("candidate_device_path") is not None and counts["device_nodes"] == 0:
            raise ApiClientError(f"Поле {label}.candidate_device_path задано без обнаруженного device node.")
        if status == "AVAILABLE" and counts["accessible_device_nodes"] == 0:
            raise ApiClientError(f"Статус {label}.status=AVAILABLE требует доступный device node.")
        if status == "DETECTED" and (
            counts["device_nodes"] == 0 or counts["accessible_device_nodes"] != 0
        ):
            raise ApiClientError(f"Статус {label}.status=DETECTED противоречит счётчикам устройств.")
        if status == "NOT_CONFIGURED" and (
            counts["device_nodes"] != 0 or counts["sysfs_controllers"] == 0
        ):
            raise ApiClientError(f"Статус {label}.status=NOT_CONFIGURED противоречит счётчикам устройств.")
    _require_string(section.get("note"), "platform.capabilities.note")


_CONTROLLER_LINK_STATUSES = {"NOT_CONFIGURED", "CONNECTING", "OK", "LINK_LOST", "ERROR"}
_CONTROLLER_LINK_STATES = {
    "NOT_CONFIGURED", "TLS_HANDSHAKE", "PROTOCOL_HANDSHAKE",
    "ESTABLISHED", "LINK_LOST", "ERROR", "STOPPED",
}


def _validate_controller_link(value: object) -> None:
    section = _require_object(value, "controller_link")
    status = section.get("status")
    state = section.get("state")
    if not isinstance(status, str) or status not in _CONTROLLER_LINK_STATUSES:
        raise ApiClientError("Поле controller_link.status содержит неизвестный статус.")
    if not isinstance(state, str) or state not in _CONTROLLER_LINK_STATES:
        raise ApiClientError("Поле controller_link.state содержит неизвестное состояние.")
    for field in ("tls_authenticated", "protocol_established"):
        if not isinstance(section.get(field), bool):
            raise ApiClientError(f"Поле controller_link.{field} должно быть boolean.")
    for field in ("telemetry_snapshots_received", "error_messages_received", "rejected_frames"):
        number = section.get(field)
        if isinstance(number, bool) or not isinstance(number, int) or not 0 <= number < (1 << 64):
            raise ApiClientError(f"Поле controller_link.{field} должно быть uint64.")
    _require_string(section.get("error_code"), "controller_link.error_code")
    if status == "OK" and (state != "ESTABLISHED" or not section["tls_authenticated"] or not section["protocol_established"]):
        raise ApiClientError("Статус controller_link=OK требует установленного TLS и протокола.")
    if status == "CONNECTING" and state not in {"TLS_HANDSHAKE", "PROTOCOL_HANDSHAKE"}:
        raise ApiClientError("Статус controller_link=CONNECTING противоречит состоянию.")
    if status == "LINK_LOST" and state != "LINK_LOST":
        raise ApiClientError("Статус controller_link=LINK_LOST противоречит состоянию.")
    if status == "ERROR" and state != "ERROR":
        raise ApiClientError("Статус controller_link=ERROR противоречит состоянию.")


_TELEMETRY_QUALITY_STATUSES = {
    "OK", "STALE", "INVALID", "UNAVAILABLE", "NOT_CONFIGURED", "UNKNOWN"
}
_TELEMETRY_VALUE_TYPES = {
    "BOOLEAN", "INTEGER", "UNSIGNED_INTEGER", "NUMBER", "TEXT"
}


def _validate_controller_telemetry(value: object) -> None:
    section = _require_object(value, "controller_telemetry")
    _validate_status_message(section, "controller_telemetry")
    status = section.get("status")
    if status not in {"PASS", "NOT_CONFIGURED", "UNAVAILABLE", "UNKNOWN", "WARN", "FAIL"}:
        raise ApiClientError("Поле controller_telemetry.status содержит неизвестный статус.")
    link_connected = section.get("link_connected")
    if not isinstance(link_connected, bool):
        raise ApiClientError("Поле controller_telemetry.link_connected должно быть boolean.")
    for field in ("source_controller_id", "controller_instance_id", "received_at_utc"):
        _require_string(section.get(field), f"controller_telemetry.{field}", nullable=True)

    sequence = section.get("sequence_number")
    max_uint64 = (1 << 64) - 1
    if sequence is not None and (
        isinstance(sequence, bool) or not isinstance(sequence, int) or not 1 <= sequence <= max_uint64
    ):
        raise ApiClientError("Поле controller_telemetry.sequence_number должно быть положительным uint64 или null.")

    received_at = section.get("received_at_utc")
    if received_at is not None:
        try:
            parsed = datetime.fromisoformat(received_at.replace("Z", "+00:00"))
        except ValueError as exc:
            raise ApiClientError("Поле controller_telemetry.received_at_utc должно содержать ISO 8601.") from exc
        if parsed.tzinfo is None or parsed.utcoffset() != timedelta(0):
            raise ApiClientError("Поле controller_telemetry.received_at_utc должно быть в UTC.")

    measurements = section.get("measurements")
    if not isinstance(measurements, list) or len(measurements) > 16:
        raise ApiClientError("Поле controller_telemetry.measurements должно содержать не более 16 измерений.")

    if sequence is None:
        if (
            section.get("source_controller_id") is not None
            or section.get("controller_instance_id") is not None
            or received_at is not None
            or measurements
        ):
            raise ApiClientError("Телеметрия без sequence не должна содержать данные измерений.")
        if status == "PASS":
            raise ApiClientError("Статус PASS невозможен без принятого снимка телеметрии.")
    else:
        if not section.get("source_controller_id") or not section.get("controller_instance_id") or received_at is None:
            raise ApiClientError("Принятый снимок должен содержать ID контроллера, instance и received_at_utc.")

    for index, item in enumerate(measurements):
        label = f"controller_telemetry.measurements[{index}]"
        measurement = _require_object(item, label)
        for field in ("sensor_id", "unit", "measured_at"):
            text = measurement.get(field)
            _require_string(text, f"{label}.{field}")
            if not text:
                raise ApiClientError(f"Поле {label}.{field} не может быть пустым.")
        value_type = measurement.get("value_type")
        if not isinstance(value_type, str) or value_type not in _TELEMETRY_VALUE_TYPES:
            raise ApiClientError(f"Поле {label}.value_type содержит неизвестный тип.")
        quality = measurement.get("quality_status")
        if not isinstance(quality, str) or quality not in _TELEMETRY_QUALITY_STATUSES:
            raise ApiClientError(f"Поле {label}.quality_status содержит неизвестное качество.")
        measured_value = measurement.get("value")
        if value_type == "BOOLEAN":
            valid_value = isinstance(measured_value, bool)
        elif value_type == "INTEGER":
            valid_value = (
                isinstance(measured_value, int) and not isinstance(measured_value, bool)
                and -(1 << 63) <= measured_value < (1 << 63)
            )
        elif value_type == "UNSIGNED_INTEGER":
            valid_value = (
                isinstance(measured_value, int) and not isinstance(measured_value, bool)
                and 0 <= measured_value <= max_uint64
            )
        elif value_type == "NUMBER":
            # Defensive Gateway fallback for non-finite binary64 values:
            # JSON null is accepted only when the measurement is INVALID.
            valid_value = (
                measured_value is None and quality == "INVALID"
            ) or (
                isinstance(measured_value, (int, float))
                and not isinstance(measured_value, bool)
                and math.isfinite(measured_value)
            )
        else:
            valid_value = isinstance(measured_value, str)
        if not valid_value:
            raise ApiClientError(f"Поле {label}.value не соответствует value_type.")

    if status == "PASS" and (not link_connected or not measurements):
        raise ApiClientError("Статус controller_telemetry=PASS требует активный канал и измерения.")


def _validate_status_message(section: dict[str, Any], field: str) -> None:
    _require_status(section.get("status"), f"{field}.status")
    _require_string(section.get("message"), f"{field}.message")


class StationApiClient:
    def __init__(
        self,
        base_url: str,
        timeout_seconds: float = 3.0,
        diagnostics_timeout_seconds: float = DEFAULT_DIAGNOSTICS_TIMEOUT_SECONDS,
    ):
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
        if isinstance(diagnostics_timeout_seconds, bool) or not isinstance(diagnostics_timeout_seconds, (int, float)) or not math.isfinite(diagnostics_timeout_seconds) or diagnostics_timeout_seconds <= 0:
            raise ValueError("Тайм-аут диагностики должен быть положительным конечным числом.")

        self.base_url = f"http://127.0.0.1:{port}"
        self.timeout_seconds = float(timeout_seconds)
        self.diagnostics_timeout_seconds = float(diagnostics_timeout_seconds)
        self._opener = urllib.request.build_opener(_NoRedirectHandler())

    def _get_json(self, route: str, *, timeout_seconds: float | None = None) -> dict[str, Any]:
        request = urllib.request.Request(
            self.base_url + route,
            headers={"Accept": "application/json", "Cache-Control": "no-cache"},
            method="GET",
        )
        try:
            with self._opener.open(
                request,
                timeout=self.timeout_seconds if timeout_seconds is None else timeout_seconds,
            ) as response:
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

    def _post_json(self, route: str, payload: dict[str, object]) -> dict[str, Any]:
        try:
            encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        except (TypeError, ValueError) as exc:
            raise ApiClientError("Тестовый запрос не удалось сериализовать в JSON.") from exc
        if not encoded or len(encoded) > 4096:
            raise ApiClientError("Размер тестового запроса должен быть от 1 до 4096 байт.")

        request = urllib.request.Request(
            self.base_url + route,
            data=encoded,
            headers={
                "Accept": "application/json",
                "Content-Type": "application/json; charset=utf-8",
                "Cache-Control": "no-store",
            },
            method="POST",
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
            payload_object = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ApiClientError("API вернул некорректный JSON.") from exc
        if not isinstance(payload_object, dict):
            raise ApiClientError("Корневой элемент JSON должен быть объектом.")
        return payload_object

    def submit_test_adapter_request(self) -> dict[str, Any]:
        """Send one harmless PING to the explicitly enabled VM test adapter."""
        request_id = uuid.uuid4().hex
        response = self._post_json(
            "/api/v1/test/adapter/request",
            {
                "schema_version": "1.0",
                "request_id": request_id,
                "operation": "PING",
            },
        )
        self.validate_test_adapter_response(response, request_id)
        return response

    @staticmethod
    def validate_test_adapter_response(payload: dict[str, Any], request_id: str) -> None:
        expected_fields = {
            "schema_version",
            "request_id",
            "status",
            "source",
            "physical_action_performed",
        }
        if set(payload) != expected_fields:
            raise ApiClientError("Ответ тестового адаптера содержит неизвестный набор полей.")
        if payload.get("schema_version") != "1.0":
            raise ApiClientError("Неизвестная версия ответа тестового адаптера.")
        if payload.get("request_id") != request_id:
            raise ApiClientError("Ответ тестового адаптера не соответствует отправленному request_id.")
        if payload.get("status") != "TEST_ADAPTER_ACCEPTED":
            raise ApiClientError("Тестовый адаптер не подтвердил приём запроса.")
        if payload.get("source") != "TEST_ADAPTER":
            raise ApiClientError("Ответ получен не от тестового адаптера.")
        if payload.get("physical_action_performed") is not False:
            raise ApiClientError("Тестовый ответ некорректно описывает выполнение физического действия.")

    def get_health(self) -> dict[str, Any]:
        payload = self._get_json("/api/v1/health")
        self.validate_health_response(payload)
        return payload

    @staticmethod
    def validate_health_response(payload: dict[str, Any]) -> None:
        if (
            payload.get("api_version") != "1"
            or payload.get("service") != "dockstation-cpp-gateway"
            or payload.get("implementation") != "cpp"
        ):
            raise ApiClientError("Ответ получен не от основного C++ Gateway или версия API неизвестна.")
        if payload.get("status") != "PASS" or payload.get("read_only") is not True:
            raise ApiClientError("Gateway не подтвердил готовность read-only API.")

    def get_system_diagnostics(self) -> dict[str, Any]:
        payload = self._get_json(
            "/api/v1/system/diagnostics",
            timeout_seconds=self.diagnostics_timeout_seconds,
        )
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
        # Capabilities is an additive extension; older VM snapshots remain valid.
        if "capabilities" in platform:
            _validate_platform_capabilities(platform.get("capabilities"))
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

        # Older VM Gateway snapshots may predate the additive resources section.
        # In that case the QML bridge displays these metrics as unavailable.
        resources: dict[str, Any] = {}
        if "resources" in platform:
            resources = _require_object(platform.get("resources"), "platform.resources")
        if resources:
            memory = _require_object(resources.get("memory"), "platform.resources.memory")
            storage = _require_object(resources.get("storage"), "platform.resources.storage")
            cpu_temperature = _require_object(resources.get("cpu_temperature"), "platform.resources.cpu_temperature")
            cpu_load = _require_object(resources.get("cpu_load"), "platform.resources.cpu_load")
            for label, section in (
                ("platform.resources.memory", memory),
                ("platform.resources.storage", storage),
                ("platform.resources.cpu_temperature", cpu_temperature),
                ("platform.resources.cpu_load", cpu_load),
            ):
                _validate_status_message(section, label)
        else:
            memory = storage = cpu_temperature = cpu_load = {}

        def validate_optional_number(value: object, field: str, *, integer: bool = False, maximum: float | None = None) -> None:
            if value is None:
                return
            valid_type = isinstance(value, int) if integer else isinstance(value, (int, float))
            if isinstance(value, bool) or not valid_type:
                raise ApiClientError(f"Поле {field} должно быть неотрицательным числом или null.")
            if isinstance(value, float) and not math.isfinite(value):
                raise ApiClientError(f"Поле {field} должно быть конечным числом.")
            if value < 0:
                raise ApiClientError(f"Поле {field} должно быть неотрицательным числом или null.")
            if maximum is not None and value > maximum:
                raise ApiClientError(f"Поле {field} выходит за допустимый диапазон.")

        max_uint64 = (1 << 64) - 1
        if resources:
            for key in ("total_bytes", "available_bytes", "used_bytes"):
                validate_optional_number(memory.get(key), f"platform.resources.memory.{key}", integer=True, maximum=max_uint64)
            validate_optional_number(memory.get("used_percent"), "platform.resources.memory.used_percent", maximum=100)
            for key in ("total_bytes", "free_bytes", "available_bytes", "used_bytes"):
                validate_optional_number(storage.get(key), f"platform.resources.storage.{key}", integer=True, maximum=max_uint64)
            validate_optional_number(storage.get("used_percent"), "platform.resources.storage.used_percent", maximum=100)
            _require_string(storage.get("path"), "platform.resources.storage.path")
            validate_optional_number(cpu_temperature.get("celsius"), "platform.resources.cpu_temperature.celsius")
            _require_string(cpu_temperature.get("sensor_type"), "platform.resources.cpu_temperature.sensor_type", nullable=True)
            for key in ("one_minute", "five_minutes", "fifteen_minutes"):
                validate_optional_number(cpu_load.get(key), f"platform.resources.cpu_load.{key}")
            for key in ("runnable_tasks", "total_tasks"):
                validate_optional_number(cpu_load.get(key), f"platform.resources.cpu_load.{key}", integer=True, maximum=max_uint64)

            if memory.get("status") == "PASS" and any(memory.get(key) is None for key in ("total_bytes", "available_bytes", "used_bytes", "used_percent")):
                raise ApiClientError("Успешная диагностика RAM должна содержать все значения.")
            if storage.get("status") == "PASS" and any(storage.get(key) is None for key in ("total_bytes", "free_bytes", "available_bytes", "used_bytes", "used_percent")):
                raise ApiClientError("Успешная диагностика файловой системы должна содержать все значения.")
            if cpu_temperature.get("status") == "PASS" and (cpu_temperature.get("celsius") is None or not cpu_temperature.get("sensor_type")):
                raise ApiClientError("Успешная диагностика температуры CPU должна содержать значение и тип датчика.")
            if cpu_load.get("status") == "PASS" and any(cpu_load.get(key) is None for key in ("one_minute", "five_minutes", "fifteen_minutes", "runnable_tasks", "total_tasks")):
                raise ApiClientError("Успешная диагностика load average должна содержать все значения.")

        # ControllerLink is additive so older read-only Gateway snapshots remain valid.
        if "controller_link" in payload:
            _validate_controller_link(payload.get("controller_link"))

        # Additive telemetry fields are absent on older VM Gateway versions.
        if "controller_telemetry" in payload:
            _validate_controller_telemetry(payload.get("controller_telemetry"))

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
