from __future__ import annotations

from dockstation_operator.models.diagnostics_model import DiagnosticsModel


def test_apply_diagnostics_marks_snapshot_current():
    model = DiagnosticsModel()
    model.apply_diagnostics({"summary": {"status": "PASS"}})
    assert model.snapshot == {"summary": {"status": "PASS"}}
    assert model.stale is False
    assert model.connection_state == "CONNECTED"
    assert model.diagnostics_age_seconds() is not None


def test_connection_loss_keeps_snapshot_but_marks_it_stale():
    model = DiagnosticsModel()
    snapshot = {"summary": {"status": "PASS"}}
    model.apply_diagnostics(snapshot)
    model.mark_diagnostics_stale("API недоступен")
    assert model.snapshot is snapshot
    assert model.stale is True
    assert model.stale_reason == "API недоступен"
    assert model.connection_state == "DEGRADED"


def test_disconnect_does_not_present_old_snapshot_as_current():
    model = DiagnosticsModel()
    model.apply_diagnostics({"summary": {"status": "PASS"}})
    model.clear_connection_state()
    assert model.stale is True
    assert model.connection_state == "DISCONNECTED"
    assert "закрыто" in model.stale_reason


def test_event_list_is_bounded():
    model = DiagnosticsModel()
    for index in range(8):
        model.append_client_event(str(index), limit=3)
    assert len(model.client_events) == 3
    assert model.client_events[-1].endswith("7")


def test_clear_connection_state_is_distinct_from_transport_degradation():
    model = DiagnosticsModel()
    model.apply_diagnostics({"summary": {"status": "PASS"}})
    model.clear_connection_state()
    assert model.connection_state == "DISCONNECTED"
    assert model.stale is True
