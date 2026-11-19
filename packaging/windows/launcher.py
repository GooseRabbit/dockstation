"""PyInstaller entry point for the packaged QML Operator."""
import os
import traceback
from pathlib import Path

try:
    from dockstation_operator.qml_app import main

    raise SystemExit(main())
except BaseException:
    log_path = os.environ.get("DOCKSTATION_SMOKE_LOG")
    if log_path:
        Path(log_path).write_text(traceback.format_exc(), encoding="utf-8")
    raise
