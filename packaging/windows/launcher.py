"""PyInstaller entry point for the packaged QML Operator."""
import os
import traceback
from pathlib import Path


def _write_startup_exception() -> None:
    log_path = os.environ.get("DOCKSTATION_SMOKE_LOG")
    if log_path:
        Path(log_path).write_text(traceback.format_exc(), encoding="utf-8")


try:
    from dockstation_operator.qml_app import main
except BaseException:
    _write_startup_exception()
    raise

if __name__ == "__main__":
    try:
        exit_code = main()
    except BaseException:
        _write_startup_exception()
        raise
    raise SystemExit(exit_code)
