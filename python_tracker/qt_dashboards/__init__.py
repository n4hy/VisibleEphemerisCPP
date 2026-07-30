"""Qt (PyQt6) dashboards for Visible Ephemeris.

Three dashboards live in this package, all built against the newest Qt binding
available on the system (PyQt6 >= 6.6, Qt >= 6.4):

    orbital_architect_qt   Satellite-selection GUI, replaces the ANSI
                           orbital_architect.py workflow.
    hpop_panel             Real-time HPOP / physics-stream diagnostic panel that
                           consumes the :12346 TCP frame feed.
    ve_ide                 VS Code-style IDE shell (activity bar, file tree,
                           tabbed editor, terminal + output dock) with the
                           tracker wired in as a run target.

Each module exposes ``main() -> int`` and can be launched with, e.g.::

    python -m python_tracker.qt_dashboards.ve_ide
    ve-ide                    # after install.sh / .deb install
"""
from __future__ import annotations

__all__ = ["theme"]
__version__ = "1.0.0"
