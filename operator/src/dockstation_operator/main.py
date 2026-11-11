"""Start the DockStation Operator desktop application."""

from __future__ import annotations

import tkinter as tk

from .ui.main_window import OperatorWindow


def main() -> int:
    root = tk.Tk()
    OperatorWindow(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
