"""Shared VS Code-style theme and reusable widgets for the Qt dashboards.

Centralising the palette here keeps the three dashboards visually consistent and
lets us change the accent colour in one place. The palette follows the widely
recognised VS Code "Dark+" defaults so users get zero-friction familiarity.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QFontDatabase, QPalette
from PyQt6.QtWidgets import (
    QApplication,
    QFrame,
    QLabel,
    QPlainTextEdit,
    QSizePolicy,
    QStatusBar,
    QVBoxLayout,
    QWidget,
)


@dataclass(frozen=True)
class Palette:
    """VS Code Dark+ palette values, in hex."""

    bg_editor: str = "#1E1E1E"
    bg_sidebar: str = "#252526"
    bg_activitybar: str = "#333333"
    bg_statusbar: str = "#007ACC"
    bg_panel: str = "#1E1E1E"
    bg_tab_active: str = "#1E1E1E"
    bg_tab_inactive: str = "#2D2D2D"
    fg_default: str = "#D4D4D4"
    fg_muted: str = "#858585"
    fg_link: str = "#3794FF"
    fg_error: str = "#F48771"
    fg_warn: str = "#CCA700"
    fg_ok: str = "#89D185"
    border: str = "#3C3C3C"
    accent: str = "#007ACC"
    selection: str = "#264F78"

    # Syntax
    syn_keyword: str = "#569CD6"
    syn_string: str = "#CE9178"
    syn_number: str = "#B5CEA8"
    syn_comment: str = "#6A9955"
    syn_type: str = "#4EC9B0"
    syn_func: str = "#DCDCAA"


PALETTE = Palette()


def monospace_font(size: int = 11) -> QFont:
    """Return a monospaced font, preferring Fira Code / JetBrains Mono if present."""
    for family in ("Fira Code", "JetBrains Mono", "Cascadia Code", "DejaVu Sans Mono", "Monospace"):
        if family in QFontDatabase.families():
            f = QFont(family, size)
            f.setStyleHint(QFont.StyleHint.Monospace)
            return f
    f = QFont()
    f.setStyleHint(QFont.StyleHint.Monospace)
    f.setPointSize(size)
    return f


def ui_font(size: int = 10) -> QFont:
    for family in ("Segoe UI", "Cantarell", "Noto Sans", "DejaVu Sans"):
        if family in QFontDatabase.families():
            f = QFont(family, size)
            return f
    return QFont("", size)


def apply_dark_theme(app: QApplication) -> None:
    """Install the dark palette and stylesheet on the given QApplication."""
    app.setStyle("Fusion")
    p = QPalette()
    p.setColor(QPalette.ColorRole.Window, QColor(PALETTE.bg_editor))
    p.setColor(QPalette.ColorRole.WindowText, QColor(PALETTE.fg_default))
    p.setColor(QPalette.ColorRole.Base, QColor(PALETTE.bg_editor))
    p.setColor(QPalette.ColorRole.AlternateBase, QColor(PALETTE.bg_sidebar))
    p.setColor(QPalette.ColorRole.Text, QColor(PALETTE.fg_default))
    p.setColor(QPalette.ColorRole.Button, QColor(PALETTE.bg_sidebar))
    p.setColor(QPalette.ColorRole.ButtonText, QColor(PALETTE.fg_default))
    p.setColor(QPalette.ColorRole.Highlight, QColor(PALETTE.selection))
    p.setColor(QPalette.ColorRole.HighlightedText, QColor("#FFFFFF"))
    p.setColor(QPalette.ColorRole.ToolTipBase, QColor(PALETTE.bg_sidebar))
    p.setColor(QPalette.ColorRole.ToolTipText, QColor(PALETTE.fg_default))
    p.setColor(QPalette.ColorRole.PlaceholderText, QColor(PALETTE.fg_muted))
    app.setPalette(p)

    app.setStyleSheet(f"""
        QWidget {{ color: {PALETTE.fg_default}; }}
        QMainWindow, QDialog {{ background: {PALETTE.bg_editor}; }}

        QMenuBar {{ background: {PALETTE.bg_sidebar}; color: {PALETTE.fg_default}; }}
        QMenuBar::item:selected {{ background: {PALETTE.selection}; }}
        QMenu {{ background: {PALETTE.bg_sidebar}; border: 1px solid {PALETTE.border}; }}
        QMenu::item:selected {{ background: {PALETTE.selection}; }}

        QToolBar {{ background: {PALETTE.bg_activitybar}; border: 0; spacing: 2px; }}
        QToolButton {{ background: transparent; border: 0; padding: 6px; color: {PALETTE.fg_default}; }}
        QToolButton:hover {{ background: {PALETTE.bg_sidebar}; }}
        QToolButton:checked {{ background: {PALETTE.bg_editor}; border-left: 2px solid {PALETTE.accent}; }}

        QStatusBar {{ background: {PALETTE.bg_statusbar}; color: #FFFFFF; }}
        QStatusBar::item {{ border: 0; }}

        QTabWidget::pane {{ border: 0; background: {PALETTE.bg_editor}; }}
        QTabBar::tab {{
            background: {PALETTE.bg_tab_inactive};
            color: {PALETTE.fg_muted};
            padding: 6px 14px;
            border: 0;
        }}
        QTabBar::tab:selected {{
            background: {PALETTE.bg_tab_active};
            color: {PALETTE.fg_default};
            border-top: 1px solid {PALETTE.accent};
        }}
        QTabBar::tab:hover {{ color: {PALETTE.fg_default}; }}

        QDockWidget {{ color: {PALETTE.fg_default}; titlebar-close-icon: none; }}
        QDockWidget::title {{
            background: {PALETTE.bg_sidebar};
            padding: 4px 8px;
            text-transform: uppercase;
            font-size: 10px;
            letter-spacing: 1px;
        }}

        QLineEdit, QPlainTextEdit, QTextEdit, QTreeView, QListView, QTableView {{
            background: {PALETTE.bg_editor};
            color: {PALETTE.fg_default};
            border: 1px solid {PALETTE.border};
            selection-background-color: {PALETTE.selection};
        }}
        QHeaderView::section {{
            background: {PALETTE.bg_sidebar};
            color: {PALETTE.fg_default};
            border: 0;
            padding: 4px;
        }}

        QPushButton {{
            background: {PALETTE.bg_sidebar};
            color: {PALETTE.fg_default};
            border: 1px solid {PALETTE.border};
            padding: 6px 14px;
        }}
        QPushButton:hover {{ background: {PALETTE.selection}; }}
        QPushButton:pressed {{ background: {PALETTE.accent}; }}
        QPushButton:disabled {{ color: {PALETTE.fg_muted}; }}

        QCheckBox, QRadioButton {{ color: {PALETTE.fg_default}; }}

        QSplitter::handle {{ background: {PALETTE.border}; }}
        QScrollBar:vertical {{ background: {PALETTE.bg_editor}; width: 12px; }}
        QScrollBar::handle:vertical {{ background: {PALETTE.border}; min-height: 30px; }}
        QScrollBar::handle:vertical:hover {{ background: {PALETTE.fg_muted}; }}
        QScrollBar:horizontal {{ background: {PALETTE.bg_editor}; height: 12px; }}
        QScrollBar::handle:horizontal {{ background: {PALETTE.border}; min-width: 30px; }}
        QScrollBar::add-line, QScrollBar::sub-line {{ background: transparent; border: 0; height: 0; width: 0; }}

        QGroupBox {{ border: 1px solid {PALETTE.border}; margin-top: 10px; padding-top: 8px; }}
        QGroupBox::title {{ subcontrol-origin: margin; left: 8px; color: {PALETTE.fg_muted}; }}
    """)
    app.setFont(ui_font(10))


class LogPane(QPlainTextEdit):
    """Read-only append-only monospaced log with ANSI-free coloured levels."""

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setFont(monospace_font(10))
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.setMaximumBlockCount(10_000)

    def _append_html(self, html: str) -> None:
        self.appendHtml(html)

    def info(self, msg: str) -> None:
        self._append_html(f'<span style="color:{PALETTE.fg_default}">{_esc(msg)}</span>')

    def ok(self, msg: str) -> None:
        self._append_html(f'<span style="color:{PALETTE.fg_ok}">[OK]  {_esc(msg)}</span>')

    def warn(self, msg: str) -> None:
        self._append_html(f'<span style="color:{PALETTE.fg_warn}">[WARN] {_esc(msg)}</span>')

    def error(self, msg: str) -> None:
        self._append_html(f'<span style="color:{PALETTE.fg_error}">[ERR] {_esc(msg)}</span>')


def _esc(s: str) -> str:
    return (
        s.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace("\n", "<br>")
    )


class SectionLabel(QLabel):
    """VS Code-style ALL-CAPS side-panel section title."""

    def __init__(self, text: str, parent: Optional[QWidget] = None):
        super().__init__(text.upper(), parent)
        f = ui_font(9)
        f.setBold(True)
        self.setFont(f)
        self.setStyleSheet(
            f"color: {PALETTE.fg_muted}; padding: 6px 10px; "
            f"letter-spacing: 1px; background: {PALETTE.bg_sidebar};"
        )
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)


class Card(QFrame):
    """Sunken card container for grouping controls; wraps a vertical layout."""

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setStyleSheet(
            f"background: {PALETTE.bg_sidebar}; border: 1px solid {PALETTE.border};"
        )
        self.vbox = QVBoxLayout(self)
        self.vbox.setContentsMargins(10, 10, 10, 10)
        self.vbox.setSpacing(6)


class StatusBar(QStatusBar):
    """Status bar with a permanent right-hand slot for connection state."""

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self._conn = QLabel("● offline")
        self._conn.setStyleSheet("color: #FFFFFF; padding: 0 8px;")
        self.addPermanentWidget(self._conn)

    def set_connection(self, connected: bool, label: str = "") -> None:
        dot = "●"
        colour = "#89D185" if connected else "#F48771"
        text = label or ("online" if connected else "offline")
        self._conn.setText(f'<span style="color:{colour}">{dot}</span> {text}')
        self._conn.setTextFormat(Qt.TextFormat.RichText)


class BusyIndicator(QWidget):
    """Tiny pulse widget shown while a background action is in flight."""

    pulsed = pyqtSignal(bool)

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self._label = QLabel("", self)
        self._label.setStyleSheet(f"color: {PALETTE.accent}; padding: 0 6px;")
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(self._label)

    def start(self, message: str) -> None:
        self._label.setText(f"⟳ {message}")
        self.pulsed.emit(True)

    def stop(self, message: str = "") -> None:
        self._label.setText(message)
        self.pulsed.emit(False)
