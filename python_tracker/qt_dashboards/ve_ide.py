"""``ve-ide`` — VS Code-style Qt IDE panel for the Visible Ephemeris project.

Layout (matches the VS Code mental model):

    ┌──┬────────────┬─────────────────────────────────────────────┐
    │A │ Side panel │ Tabbed editor area                          │
    │c │ (Explorer/ │  ├── file1.cpp  ├── main.py                 │
    │t │ Search/    │  │                                          │
    │  │ Run/       │  │                                          │
    │b │ Satellites │  │                                          │
    │a │ /Settings) │  │                                          │
    │r │            │  │                                          │
    │  ├────────────┴─────────────────────────────────────────────┤
    │  │ Bottom dock: Terminal │ Output │ Problems │ Satellites   │
    ├──┴──────────────────────────────────────────────────────────┤
    │ Status bar (blue): branch · errors · warnings · tracker on  │
    └─────────────────────────────────────────────────────────────┘

Ergonomic features:
    * Ctrl+P            quick-open file
    * Ctrl+Shift+P      command palette
    * Ctrl+`            focus terminal
    * F5                run tracker (foreground process; stdout → Output)
    * Ctrl+Shift+B      run build (cmake --build build)
    * Ctrl+Shift+F      focus search panel
    * Ctrl+W / Ctrl+Tab close / cycle editor tabs
    * Editor tabs are auto-created for files opened from the tree
    * Files are opened read-write; save via Ctrl+S, dirty tabs show a '●'
    * Syntax highlighting for Python, C++, YAML (minimal but recognisable)
    * Satellites tab in the bottom dock consumes the physics stream and shows
      the same live table as :12346, so the tracker's progress is visible
      inside the IDE without switching windows.

All heavy work runs on background QThreads or QProcess so the UI stays
responsive.
"""
from __future__ import annotations

import fnmatch
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional

from PyQt6.QtCore import (
    QDir,
    QModelIndex,
    QProcess,
    QSize,
    Qt,
    QThread,
    pyqtSignal,
)
from PyQt6.QtGui import (
    QAction,
    QColor,
    QFileSystemModel,
    QFont,
    QIcon,
    QKeySequence,
    QPixmap,
    QSyntaxHighlighter,
    QTextCharFormat,
    QTextCursor,
)
from PyQt6.QtWidgets import (
    QApplication,
    QComboBox,
    QDialog,
    QDockWidget,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMenuBar,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSizePolicy,
    QSplitter,
    QStackedWidget,
    QStatusBar,
    QTabWidget,
    QTableWidget,
    QTableWidgetItem,
    QTextEdit,
    QToolBar,
    QToolButton,
    QTreeView,
    QVBoxLayout,
    QWidget,
)

from .hpop_panel import Frame, PhysicsClient
from .theme import PALETTE, LogPane, StatusBar, apply_dark_theme, monospace_font, ui_font


# ---------------------------------------------------------------------------
# Syntax highlighters — deliberately small but recognisable.
# ---------------------------------------------------------------------------

class _Rule:
    __slots__ = ("pattern", "fmt")
    def __init__(self, pattern: str, fmt: QTextCharFormat):
        self.pattern = re.compile(pattern)
        self.fmt = fmt


def _fmt(color: str, bold: bool = False, italic: bool = False) -> QTextCharFormat:
    f = QTextCharFormat()
    f.setForeground(QColor(color))
    if bold:
        f.setFontWeight(QFont.Weight.Bold)
    f.setFontItalic(italic)
    return f


class LangHighlighter(QSyntaxHighlighter):
    def __init__(self, doc, rules: List[_Rule]):
        super().__init__(doc)
        self._rules = rules

    def highlightBlock(self, text: str) -> None:
        for rule in self._rules:
            for m in rule.pattern.finditer(text):
                self.setFormat(m.start(), m.end() - m.start(), rule.fmt)


def python_rules() -> List[_Rule]:
    kw = r"\b(?:def|class|import|from|as|return|if|elif|else|for|while|in|not|and|or|is|None|True|False|try|except|finally|raise|with|yield|lambda|pass|break|continue|global|nonlocal|assert)\b"
    return [
        _Rule(r"#[^\n]*", _fmt(PALETTE.syn_comment, italic=True)),
        _Rule(r"(\"\"\"|''')(?:[^\\]|\\.)*?\1", _fmt(PALETTE.syn_string)),
        _Rule(r'"(?:[^"\\]|\\.)*"', _fmt(PALETTE.syn_string)),
        _Rule(r"'(?:[^'\\]|\\.)*'", _fmt(PALETTE.syn_string)),
        _Rule(r"\b\d+\.?\d*\b", _fmt(PALETTE.syn_number)),
        _Rule(kw, _fmt(PALETTE.syn_keyword, bold=True)),
        _Rule(r"\b[A-Z][A-Za-z0-9_]*\b", _fmt(PALETTE.syn_type)),
        _Rule(r"\bdef\s+([A-Za-z_][A-Za-z0-9_]*)", _fmt(PALETTE.syn_func)),
    ]


def cpp_rules() -> List[_Rule]:
    kw = (r"\b(?:auto|bool|break|case|catch|char|class|const|constexpr|continue|default|delete|do|double|else|enum|explicit|export|extern|false|float|for|friend|goto|if|inline|int|long|mutable|namespace|new|noexcept|nullptr|operator|private|protected|public|register|return|short|signed|sizeof|static|struct|switch|template|this|throw|true|try|typedef|typename|union|unsigned|using|virtual|void|volatile|while)\b")
    return [
        _Rule(r"//[^\n]*", _fmt(PALETTE.syn_comment, italic=True)),
        _Rule(r"/\*.*?\*/", _fmt(PALETTE.syn_comment, italic=True)),
        _Rule(r'"(?:[^"\\]|\\.)*"', _fmt(PALETTE.syn_string)),
        _Rule(r"'(?:[^'\\]|\\.)*'", _fmt(PALETTE.syn_string)),
        _Rule(r"\b\d+\.?\d*[fFuUlL]?\b", _fmt(PALETTE.syn_number)),
        _Rule(r"^\s*#\w+", _fmt(PALETTE.syn_keyword, bold=True)),
        _Rule(kw, _fmt(PALETTE.syn_keyword, bold=True)),
        _Rule(r"\b[A-Z][A-Za-z0-9_]*\b", _fmt(PALETTE.syn_type)),
    ]


def yaml_rules() -> List[_Rule]:
    return [
        _Rule(r"#[^\n]*", _fmt(PALETTE.syn_comment, italic=True)),
        _Rule(r"^\s*[A-Za-z0-9_\-.]+\s*:", _fmt(PALETTE.syn_keyword)),
        _Rule(r'"(?:[^"\\]|\\.)*"', _fmt(PALETTE.syn_string)),
        _Rule(r"'(?:[^'\\]|\\.)*'", _fmt(PALETTE.syn_string)),
        _Rule(r"\b-?\d+\.?\d*\b", _fmt(PALETTE.syn_number)),
        _Rule(r"\b(?:true|false|null|yes|no)\b", _fmt(PALETTE.syn_number, bold=True)),
    ]


def _rules_for(path: str) -> Optional[List[_Rule]]:
    ext = os.path.splitext(path)[1].lower()
    if ext in (".py",):
        return python_rules()
    if ext in (".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx", ".h", ".c"):
        return cpp_rules()
    if ext in (".yaml", ".yml"):
        return yaml_rules()
    return None


# ---------------------------------------------------------------------------
# Editor tab
# ---------------------------------------------------------------------------

class EditorTab(QPlainTextEdit):
    dirty_changed = pyqtSignal(bool)

    def __init__(self, path: Optional[str], text: str, parent=None):
        super().__init__(parent)
        self.setFont(monospace_font(11))
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.setTabStopDistance(4 * self.fontMetrics().horizontalAdvance(" "))
        self.path = path
        self._dirty = False
        self.setPlainText(text)
        self._highlighter = None
        if path:
            rules = _rules_for(path)
            if rules:
                self._highlighter = LangHighlighter(self.document(), rules)
        self.textChanged.connect(self._on_text_changed)

    def _on_text_changed(self) -> None:
        if not self._dirty:
            self._dirty = True
            self.dirty_changed.emit(True)

    def mark_saved(self) -> None:
        if self._dirty:
            self._dirty = False
            self.dirty_changed.emit(False)

    def is_dirty(self) -> bool:
        return self._dirty


# ---------------------------------------------------------------------------
# Side-panel views
# ---------------------------------------------------------------------------

class ExplorerView(QWidget):
    file_opened = pyqtSignal(str)

    def __init__(self, root: str, parent=None):
        super().__init__(parent)
        v = QVBoxLayout(self)
        v.setContentsMargins(0, 0, 0, 0); v.setSpacing(0)
        header = QLabel("EXPLORER")
        header.setStyleSheet(
            f"color: {PALETTE.fg_muted}; padding: 6px 10px; letter-spacing: 1px;"
            f"background: {PALETTE.bg_sidebar}; font-weight: bold;"
        )
        v.addWidget(header)

        self.model = QFileSystemModel(self)
        self.model.setRootPath(root)
        self.model.setNameFilters(["*"])
        self.model.setNameFilterDisables(False)
        self.tree = QTreeView(self)
        self.tree.setModel(self.model)
        self.tree.setRootIndex(self.model.index(root))
        self.tree.setHeaderHidden(True)
        for c in range(1, 4):
            self.tree.hideColumn(c)
        self.tree.setAnimated(True)
        self.tree.setIndentation(14)
        self.tree.doubleClicked.connect(self._open)
        v.addWidget(self.tree, 1)

    def _open(self, idx: QModelIndex) -> None:
        info = self.model.fileInfo(idx)
        if info.isFile():
            self.file_opened.emit(info.absoluteFilePath())


class SearchView(QWidget):
    file_opened = pyqtSignal(str, int)  # path, line-number

    def __init__(self, root: str, parent=None):
        super().__init__(parent)
        self.root = root
        v = QVBoxLayout(self)
        v.setContentsMargins(0, 0, 0, 0); v.setSpacing(4)
        v.addWidget(QLabel("SEARCH IN FILES", styleSheet=f"color:{PALETTE.fg_muted};padding:6px 10px;letter-spacing:1px;background:{PALETTE.bg_sidebar};font-weight:bold;"))
        self.query = QLineEdit(); self.query.setPlaceholderText("regex…"); v.addWidget(self.query)
        self.include = QLineEdit(); self.include.setPlaceholderText("include glob (e.g. *.py)"); v.addWidget(self.include)
        row = QHBoxLayout()
        self.go = QPushButton("Search"); row.addWidget(self.go)
        self.count = QLabel(""); self.count.setStyleSheet(f"color:{PALETTE.fg_muted};"); row.addWidget(self.count); row.addStretch(1)
        v.addLayout(row)
        self.results = QListWidget(); self.results.setFont(monospace_font(9)); v.addWidget(self.results, 1)
        self.go.clicked.connect(self._search)
        self.query.returnPressed.connect(self._search)
        self.results.itemActivated.connect(self._on_activate)

    def _search(self) -> None:
        q = self.query.text().strip()
        if not q:
            self.results.clear(); self.count.setText(""); return
        try:
            rx = re.compile(q)
        except re.error as e:
            QMessageBox.warning(self, "Bad regex", str(e)); return
        include = self.include.text().strip() or "*"
        skip_dirs = {".git", "build", "venv", "__pycache__", "node_modules"}
        self.results.clear()
        hits = 0
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames[:] = [d for d in dirnames if d not in skip_dirs]
            for fn in filenames:
                if not fnmatch.fnmatch(fn, include):
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    with open(path, "r", encoding="utf-8", errors="ignore") as f:
                        for lineno, line in enumerate(f, start=1):
                            if rx.search(line):
                                rel = os.path.relpath(path, self.root)
                                item = QListWidgetItem(f"{rel}:{lineno}  {line.rstrip()[:120]}")
                                item.setData(Qt.ItemDataRole.UserRole, (path, lineno))
                                self.results.addItem(item)
                                hits += 1
                                if hits > 500:
                                    self.count.setText("500+ hits (truncated)")
                                    return
                except OSError:
                    continue
        self.count.setText(f"{hits} hit{'s' if hits != 1 else ''}")

    def _on_activate(self, item: QListWidgetItem) -> None:
        data = item.data(Qt.ItemDataRole.UserRole)
        if data:
            path, lineno = data
            self.file_opened.emit(path, lineno)


def _project_python(root: str) -> str:
    """Return the project's venv python if one exists under ``root``, else the
    interpreter running us. Prefer venv so the tracker's skyfield/fastapi/
    uvicorn deps are on sys.path without the user having to activate anything.
    """
    for candidate in ("venv", ".venv", "env"):
        p = os.path.join(root, candidate, "bin", "python3")
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return sys.executable


class RunView(QWidget):
    run_requested = pyqtSignal(str, list)  # program, args

    def __init__(self, root: str, parent=None):
        super().__init__(parent)
        self.root = root
        self._py = _project_python(root)
        v = QVBoxLayout(self)
        v.setContentsMargins(0, 0, 0, 0); v.setSpacing(6)
        v.addWidget(QLabel("RUN", styleSheet=f"color:{PALETTE.fg_muted};padding:6px 10px;letter-spacing:1px;background:{PALETTE.bg_sidebar};font-weight:bold;"))
        note = QLabel(f"python: {self._py}")
        note.setStyleSheet(f"color:{PALETTE.fg_muted}; padding: 0 10px 4px; font-size: 10px;")
        note.setWordWrap(True)
        v.addWidget(note)

        def button(label: str, cmd: str, args: list[str]) -> QPushButton:
            b = QPushButton(label)
            b.clicked.connect(lambda: self.run_requested.emit(cmd, args))
            return b

        for b in (
            button("▶ Run tracker (python)", self._py, ["-m", "python_tracker.main"]),
            button("▶ Run tracker with --hpop", self._py, ["-m", "python_tracker.main", "--hpop"]),
            button("▶ Orbital Architect (Qt)", self._py, ["-m", "python_tracker.qt_dashboards.orbital_architect_qt"]),
            button("▶ HPOP diagnostic panel", self._py, ["-m", "python_tracker.qt_dashboards.hpop_panel"]),
            button("▶ CMake configure", "cmake", ["-S", ".", "-B", "build"]),
            button("▶ CMake build", "cmake", ["--build", "build", "-j"]),
            button("▶ ctest", "ctest", ["--test-dir", "build", "--output-on-failure"]),
            button("▶ Run C++ VisibleEphemeris", "./build/VisibleEphemeris", []),
        ):
            v.addWidget(b)
        v.addStretch(1)
        v.addWidget(QLabel("Custom command:"))
        row = QHBoxLayout()
        self.custom = QLineEdit(); self.custom.setPlaceholderText("e.g. pytest tests/"); row.addWidget(self.custom, 1)
        self.custom_btn = QPushButton("Run"); row.addWidget(self.custom_btn); v.addLayout(row)
        self.custom_btn.clicked.connect(self._run_custom)
        self.custom.returnPressed.connect(self._run_custom)

    def _run_custom(self) -> None:
        cmd = self.custom.text().strip()
        if not cmd:
            return
        parts = shlex.split(cmd)
        self.run_requested.emit(parts[0], parts[1:])


class SettingsView(QWidget):
    def __init__(self, on_pick_root, parent=None):
        super().__init__(parent)
        v = QVBoxLayout(self)
        v.setContentsMargins(0, 0, 0, 0); v.setSpacing(6)
        v.addWidget(QLabel("SETTINGS", styleSheet=f"color:{PALETTE.fg_muted};padding:6px 10px;letter-spacing:1px;background:{PALETTE.bg_sidebar};font-weight:bold;"))
        b = QPushButton("Change project root…")
        b.clicked.connect(on_pick_root)
        v.addWidget(b)
        v.addWidget(QLabel("Font size:"))
        self.font_pt = QComboBox()
        for s in (9, 10, 11, 12, 13, 14, 16):
            self.font_pt.addItem(str(s), s)
        self.font_pt.setCurrentText("11")
        v.addWidget(self.font_pt)
        v.addStretch(1)


class SatellitesView(QWidget):
    """Bottom-dock live satellite table fed by the physics stream."""

    def __init__(self, parent=None):
        super().__init__(parent)
        v = QVBoxLayout(self); v.setContentsMargins(0, 0, 0, 0)
        top = QHBoxLayout()
        top.addWidget(QLabel("Host"))
        self.host = QLineEdit("127.0.0.1"); top.addWidget(self.host)
        top.addWidget(QLabel("Port"))
        self.port = QLineEdit("12346"); self.port.setMaximumWidth(80); top.addWidget(self.port)
        self.connect_btn = QPushButton("Connect"); top.addWidget(self.connect_btn)
        top.addStretch(1)
        self.status = QLabel("● offline"); self.status.setStyleSheet("color:#F48771;padding:0 8px;"); top.addWidget(self.status)
        v.addLayout(top)

        self.table = QTableWidget()
        self.table.setColumnCount(6)
        self.table.setHorizontalHeaderLabels(["Name", "NORAD", "El°", "Range km", "RR", "Vis"])
        self.table.setFont(monospace_font(10))
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.setSortingEnabled(True)
        v.addWidget(self.table, 1)

        self.client: Optional[PhysicsClient] = None
        self.connect_btn.clicked.connect(self._toggle)

    def _toggle(self) -> None:
        if self.client is None:
            try:
                port = int(self.port.text())
            except ValueError:
                port = 12346
            self.client = PhysicsClient(self.host.text() or "127.0.0.1", port)
            self.client.frame_received.connect(self._on_frame)
            self.client.status_changed.connect(self._on_status)
            self.client.start()
            self.connect_btn.setText("Disconnect")
        else:
            try:
                self.client.stop(); self.client.wait(1500)
            finally:
                self.client = None
            self.connect_btn.setText("Connect")
            self._on_status(False, "offline")

    def _on_frame(self, frame: Frame) -> None:
        self.table.setSortingEnabled(False)
        self.table.setRowCount(len(frame.sats))
        for row, s in enumerate(frame.sats):
            for col, val in enumerate((s.name, s.norad, f"{s.el:.1f}", f"{s.range_km:.1f}", f"{s.range_rate:.3f}", s.vis)):
                item = QTableWidgetItem(str(val))
                if col in (1, 2, 3, 4):
                    item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                self.table.setItem(row, col, item)
        self.table.setSortingEnabled(True)

    def _on_status(self, connected: bool, msg: str) -> None:
        dot = "●"
        color = "#89D185" if connected else "#F48771"
        self.status.setText(f'<span style="color:{color}">{dot}</span> {msg}')
        self.status.setTextFormat(Qt.TextFormat.RichText)


# ---------------------------------------------------------------------------
# Command palette
# ---------------------------------------------------------------------------

@dataclass
class Command:
    label: str
    shortcut: str
    handler: object


class CommandPalette(QDialog):
    def __init__(self, commands: List[Command], parent=None):
        super().__init__(parent)
        self.setWindowFlags(Qt.WindowType.Popup)
        self.setModal(True)
        self.resize(560, 380)
        v = QVBoxLayout(self)
        v.setContentsMargins(6, 6, 6, 6); v.setSpacing(4)
        self.query = QLineEdit(); self.query.setPlaceholderText("Type command…"); v.addWidget(self.query)
        self.list = QListWidget(); v.addWidget(self.list, 1)
        self._commands = commands
        self._populate("")
        self.query.textChanged.connect(self._populate)
        self.list.itemActivated.connect(self._run)
        self.query.returnPressed.connect(self._activate_first)

    def _populate(self, text: str) -> None:
        self.list.clear()
        t = text.lower()
        for c in self._commands:
            if not t or t in c.label.lower():
                item = QListWidgetItem(f"{c.label}    {c.shortcut}")
                item.setData(Qt.ItemDataRole.UserRole, c)
                self.list.addItem(item)
        if self.list.count():
            self.list.setCurrentRow(0)

    def _activate_first(self) -> None:
        if self.list.count():
            self._run(self.list.currentItem())

    def _run(self, item: QListWidgetItem) -> None:
        c: Command = item.data(Qt.ItemDataRole.UserRole)
        self.accept()
        c.handler()


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

class IDEWindow(QMainWindow):
    ACTIVITIES = ("Explorer", "Search", "Run", "Satellites", "Settings")

    def __init__(self, root: str):
        super().__init__()
        self.setWindowTitle("ve-ide — Visible Ephemeris")
        self.resize(1500, 950)
        self.root = os.path.abspath(root)
        self._open_tabs: Dict[str, EditorTab] = {}
        self._process: Optional[QProcess] = None

        self._build_activity_bar()
        self._build_side_stack()
        self._build_editor_area()
        self._build_bottom_dock()
        self._build_menu()
        self._build_status()

        # Restore default: show Explorer.
        self._activate("Explorer")

        # Global shortcuts / palette.
        self._commands = self._register_commands()
        pal = QAction("Command palette", self)
        pal.setShortcut(QKeySequence("Ctrl+Shift+P"))
        pal.triggered.connect(self._open_palette)
        self.addAction(pal)

        quick = QAction("Quick open", self)
        quick.setShortcut(QKeySequence("Ctrl+P"))
        quick.triggered.connect(self._quick_open)
        self.addAction(quick)

        focus_term = QAction("Focus terminal", self)
        focus_term.setShortcut(QKeySequence("Ctrl+`"))
        focus_term.triggered.connect(lambda: self.bottom_tabs.setCurrentWidget(self.terminal))
        self.addAction(focus_term)

    # ------------------------------------------------------------------
    def _build_activity_bar(self) -> None:
        tb = QToolBar("Activity")
        tb.setMovable(False)
        tb.setOrientation(Qt.Orientation.Vertical)
        tb.setIconSize(QSize(24, 24))
        tb.setStyleSheet(f"background: {PALETTE.bg_activitybar};")
        self._activity_actions: Dict[str, QAction] = {}
        for name in self.ACTIVITIES:
            a = QAction(_letter_icon(name[0]), name, self)
            a.setCheckable(True)
            a.setToolTip(name)
            a.triggered.connect(lambda checked, n=name: self._activate(n))
            tb.addAction(a)
            self._activity_actions[name] = a
        self.addToolBar(Qt.ToolBarArea.LeftToolBarArea, tb)

    def _build_side_stack(self) -> None:
        self.side_stack = QStackedWidget()
        self.side_stack.setMinimumWidth(240)
        self.explorer = ExplorerView(self.root)
        self.explorer.file_opened.connect(self.open_file)
        self.search_view = SearchView(self.root)
        self.search_view.file_opened.connect(lambda p, ln: self.open_file(p, line=ln))
        self.run_view = RunView(self.root)
        self.run_view.run_requested.connect(self._run_program)
        self.settings_view = SettingsView(self._pick_root)
        self.settings_view.font_pt.currentTextChanged.connect(self._change_font_size)
        # Placeholder for the Satellites activity — we already have SatellitesView in the bottom dock;
        # activating "Satellites" in the sidebar just focuses that bottom tab.
        self.sat_side_placeholder = QLabel("Satellite feed is shown in the bottom panel →")
        self.sat_side_placeholder.setStyleSheet(f"color:{PALETTE.fg_muted}; padding: 12px;")
        self.sat_side_placeholder.setWordWrap(True)
        for w in (self.explorer, self.search_view, self.run_view, self.sat_side_placeholder, self.settings_view):
            self.side_stack.addWidget(w)

        self._side_dock = QDockWidget("Side", self)
        self._side_dock.setTitleBarWidget(QWidget())  # hide title bar
        self._side_dock.setFeatures(QDockWidget.DockWidgetFeature.NoDockWidgetFeatures)
        self._side_dock.setWidget(self.side_stack)
        self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, self._side_dock)

    def _build_editor_area(self) -> None:
        self.editor_tabs = QTabWidget()
        self.editor_tabs.setTabsClosable(True)
        self.editor_tabs.setMovable(True)
        self.editor_tabs.setDocumentMode(True)
        self.editor_tabs.tabCloseRequested.connect(self._close_tab)
        self.setCentralWidget(self.editor_tabs)

        # A welcome page so an empty IDE still looks alive.
        welcome = QTextEdit()
        welcome.setReadOnly(True)
        welcome.setFont(monospace_font(11))
        welcome.setHtml(_welcome_html(self.root))
        self.editor_tabs.addTab(welcome, "Welcome")

    def _build_bottom_dock(self) -> None:
        self.bottom_tabs = QTabWidget()
        self.bottom_tabs.setDocumentMode(True)

        self.terminal = _Terminal(self.root, self)
        self.output = LogPane(self); self.output.setPlaceholderText("Program output will appear here…")
        self.problems = LogPane(self)
        self.sat_feed = SatellitesView(self)

        self.bottom_tabs.addTab(self.terminal, "Terminal")
        self.bottom_tabs.addTab(self.output, "Output")
        self.bottom_tabs.addTab(self.problems, "Problems")
        self.bottom_tabs.addTab(self.sat_feed, "Satellites")

        dock = QDockWidget("Panel", self)
        dock.setAllowedAreas(Qt.DockWidgetArea.BottomDockWidgetArea)
        dock.setTitleBarWidget(QWidget())
        dock.setFeatures(QDockWidget.DockWidgetFeature.NoDockWidgetFeatures)
        dock.setWidget(self.bottom_tabs)
        self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, dock)
        self.resizeDocks([dock], [260], Qt.Orientation.Vertical)
        self._bottom_dock = dock

    def _build_menu(self) -> None:
        mb: QMenuBar = self.menuBar()

        def add(menu, label, slot, shortcut=None):
            act = QAction(label, self)
            if shortcut:
                act.setShortcut(QKeySequence(shortcut))
            act.triggered.connect(slot)
            menu.addAction(act)
            return act

        m_file = mb.addMenu("&File")
        add(m_file, "New file", self._new_file, "Ctrl+N")
        add(m_file, "Open file…", self._open_dialog, "Ctrl+O")
        add(m_file, "Save", self.save_current, "Ctrl+S")
        add(m_file, "Save all", self.save_all, "Ctrl+Shift+S")
        m_file.addSeparator()
        add(m_file, "Open project root…", self._pick_root)
        m_file.addSeparator()
        add(m_file, "Quit", self.close, "Ctrl+Q")

        m_edit = mb.addMenu("&Edit")
        add(m_edit, "Find in files", lambda: self._activate("Search"), "Ctrl+Shift+F")

        m_view = mb.addMenu("&View")
        add(m_view, "Toggle side panel", lambda: self._side_dock.setVisible(not self._side_dock.isVisible()), "Ctrl+B")
        add(m_view, "Toggle bottom panel", lambda: self._bottom_dock.setVisible(not self._bottom_dock.isVisible()), "Ctrl+J")
        add(m_view, "Command palette", self._open_palette, "Ctrl+Shift+P")

        m_run = mb.addMenu("&Run")
        add(m_run, "Run tracker (F5)", lambda: self._run_program(_project_python(self.root), ["-m", "python_tracker.main"]), "F5")
        add(m_run, "Build (Ctrl+Shift+B)", lambda: self._run_program("cmake", ["--build", "build", "-j"]), "Ctrl+Shift+B")
        add(m_run, "ctest", lambda: self._run_program("ctest", ["--test-dir", "build", "--output-on-failure"]))
        m_run.addSeparator()
        add(m_run, "Stop running process", self._stop_process, "Ctrl+.")

        m_help = mb.addMenu("&Help")
        add(m_help, "About", self._about)

    def _build_status(self) -> None:
        self.status = StatusBar()
        self.setStatusBar(self.status)
        self.status.showMessage(f"Root: {self.root}")

    # ------------------------------------------------------------------
    def _activate(self, name: str) -> None:
        for k, act in self._activity_actions.items():
            act.setChecked(k == name)
        idx = self.ACTIVITIES.index(name)
        self.side_stack.setCurrentIndex(idx)
        self._side_dock.setVisible(True)
        if name == "Satellites":
            self.bottom_tabs.setCurrentWidget(self.sat_feed)

    def _pick_root(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "Choose project root", self.root)
        if not d:
            return
        self.root = d
        self.explorer.model.setRootPath(d)
        self.explorer.tree.setRootIndex(self.explorer.model.index(d))
        self.search_view.root = d
        self.run_view.root = d
        self.terminal.set_cwd(d)
        self.status.showMessage(f"Root: {self.root}")

    def _change_font_size(self, text: str) -> None:
        try:
            pt = int(text)
        except ValueError:
            return
        f = monospace_font(pt)
        for i in range(self.editor_tabs.count()):
            w = self.editor_tabs.widget(i)
            if isinstance(w, EditorTab):
                w.setFont(f)

    # ------------------------------------------------------------------
    def open_file(self, path: str, line: int = 0) -> None:
        path = os.path.abspath(path)
        if path in self._open_tabs:
            tab = self._open_tabs[path]
            self.editor_tabs.setCurrentWidget(tab)
            if line:
                self._goto_line(tab, line)
            return
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as e:
            QMessageBox.critical(self, "Open failed", str(e)); return
        tab = EditorTab(path, text, self)
        title = os.path.basename(path) or path
        self._open_tabs[path] = tab
        idx = self.editor_tabs.addTab(tab, title)
        self.editor_tabs.setCurrentIndex(idx)
        tab.dirty_changed.connect(lambda d, t=tab: self._set_tab_dirty(t, d))
        save = QAction(tab)
        save.setShortcut(QKeySequence("Ctrl+S"))
        save.triggered.connect(self.save_current)
        tab.addAction(save)
        if line:
            self._goto_line(tab, line)

    def _goto_line(self, tab: EditorTab, line: int) -> None:
        block = tab.document().findBlockByNumber(max(0, line - 1))
        if block.isValid():
            cursor = QTextCursor(block)
            tab.setTextCursor(cursor)
            tab.centerCursor()

    def _set_tab_dirty(self, tab: EditorTab, dirty: bool) -> None:
        idx = self.editor_tabs.indexOf(tab)
        if idx < 0:
            return
        title = os.path.basename(tab.path) if tab.path else "untitled"
        self.editor_tabs.setTabText(idx, f"● {title}" if dirty else title)

    def _close_tab(self, idx: int) -> None:
        w = self.editor_tabs.widget(idx)
        if isinstance(w, EditorTab) and w.is_dirty():
            r = QMessageBox.question(self, "Unsaved changes", f"Save changes to {os.path.basename(w.path or 'untitled')}?",
                                     QMessageBox.StandardButton.Save | QMessageBox.StandardButton.Discard | QMessageBox.StandardButton.Cancel)
            if r == QMessageBox.StandardButton.Cancel:
                return
            if r == QMessageBox.StandardButton.Save:
                self._save(w)
        if isinstance(w, EditorTab) and w.path:
            self._open_tabs.pop(w.path, None)
        self.editor_tabs.removeTab(idx)

    def save_current(self) -> None:
        w = self.editor_tabs.currentWidget()
        if isinstance(w, EditorTab):
            self._save(w)

    def save_all(self) -> None:
        for i in range(self.editor_tabs.count()):
            w = self.editor_tabs.widget(i)
            if isinstance(w, EditorTab) and w.is_dirty():
                self._save(w)

    def _save(self, tab: EditorTab) -> None:
        if not tab.path:
            path, _ = QFileDialog.getSaveFileName(self, "Save as", self.root)
            if not path:
                return
            tab.path = path
        try:
            with open(tab.path, "w", encoding="utf-8") as f:
                f.write(tab.toPlainText())
        except OSError as e:
            QMessageBox.critical(self, "Save failed", str(e))
            return
        tab.mark_saved()
        idx = self.editor_tabs.indexOf(tab)
        if idx >= 0:
            self.editor_tabs.setTabText(idx, os.path.basename(tab.path))
        self.status.showMessage(f"Saved {tab.path}", 3000)

    def _new_file(self) -> None:
        tab = EditorTab(None, "", self)
        idx = self.editor_tabs.addTab(tab, "untitled")
        self.editor_tabs.setCurrentIndex(idx)
        tab.dirty_changed.connect(lambda d, t=tab: self._set_tab_dirty(t, d))

    def _open_dialog(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Open file", self.root)
        if path:
            self.open_file(path)

    # ------------------------------------------------------------------
    # Process runner (writes stdout/stderr into Output)
    # ------------------------------------------------------------------
    def _run_program(self, program: str, args: List[str]) -> None:
        if self._process is not None and self._process.state() != QProcess.ProcessState.NotRunning:
            r = QMessageBox.question(self, "Process running",
                                     "A process is already running. Stop it first?")
            if r != QMessageBox.StandardButton.Yes:
                return
            self._stop_process()

        self.bottom_tabs.setCurrentWidget(self.output)
        self.output.info(f"$ {program} {' '.join(shlex.quote(a) for a in args)}")
        p = QProcess(self)
        p.setWorkingDirectory(self.root)
        p.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        p.readyReadStandardOutput.connect(lambda: self.output.info(bytes(p.readAllStandardOutput()).decode("utf-8", errors="replace")))
        p.finished.connect(lambda code, _s: self._on_finished(program, code))
        p.errorOccurred.connect(lambda err: self.output.error(f"process error: {err}"))
        p.start(program, args)
        self._process = p
        self.status.showMessage(f"Running {program}…")

    def _on_finished(self, program: str, code: int) -> None:
        method = self.output.ok if code == 0 else self.output.error
        method(f"[{program}] exited with code {code}")
        self.status.showMessage(f"{program} exited ({code})", 5000)

    def _stop_process(self) -> None:
        if self._process and self._process.state() != QProcess.ProcessState.NotRunning:
            self._process.terminate()
            if not self._process.waitForFinished(1500):
                self._process.kill()
            self.output.warn("process terminated")

    # ------------------------------------------------------------------
    def _register_commands(self) -> List[Command]:
        py = _project_python(self.root)
        return [
            Command("File: New", "Ctrl+N", self._new_file),
            Command("File: Open…", "Ctrl+O", self._open_dialog),
            Command("File: Save", "Ctrl+S", self.save_current),
            Command("File: Save all", "Ctrl+Shift+S", self.save_all),
            Command("View: Toggle side panel", "Ctrl+B", lambda: self._side_dock.setVisible(not self._side_dock.isVisible())),
            Command("View: Toggle bottom panel", "Ctrl+J", lambda: self._bottom_dock.setVisible(not self._bottom_dock.isVisible())),
            Command("View: Focus terminal", "Ctrl+`", lambda: self.bottom_tabs.setCurrentWidget(self.terminal)),
            Command("Explorer: Focus", "", lambda: self._activate("Explorer")),
            Command("Search: Focus", "Ctrl+Shift+F", lambda: self._activate("Search")),
            Command("Run: Tracker", "F5", lambda: self._run_program(py, ["-m", "python_tracker.main"])),
            Command("Run: Tracker with --hpop", "", lambda: self._run_program(py, ["-m", "python_tracker.main", "--hpop"])),
            Command("Run: CMake configure", "", lambda: self._run_program("cmake", ["-S", ".", "-B", "build"])),
            Command("Run: CMake build", "Ctrl+Shift+B", lambda: self._run_program("cmake", ["--build", "build", "-j"])),
            Command("Run: ctest", "", lambda: self._run_program("ctest", ["--test-dir", "build", "--output-on-failure"])),
            Command("Run: Stop process", "Ctrl+.", self._stop_process),
            Command("Dashboards: Orbital Architect", "", lambda: self._run_program(py, ["-m", "python_tracker.qt_dashboards.orbital_architect_qt"])),
            Command("Dashboards: HPOP panel", "", lambda: self._run_program(py, ["-m", "python_tracker.qt_dashboards.hpop_panel"])),
        ]

    def _open_palette(self) -> None:
        dlg = CommandPalette(self._commands, self)
        # Position under menu bar.
        dlg.move(self.mapToGlobal(self.rect().center()) - dlg.rect().center())
        dlg.exec()

    def _quick_open(self) -> None:
        # Reuse the command palette code with a file list.
        candidates: List[Command] = []
        skip_dirs = {".git", "build", "venv", "__pycache__", "node_modules"}
        for dp, dn, fn in os.walk(self.root):
            dn[:] = [d for d in dn if d not in skip_dirs]
            for f in fn:
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, self.root)
                candidates.append(Command(rel, "", lambda pp=p: self.open_file(pp)))
                if len(candidates) > 2000:
                    break
        dlg = CommandPalette(candidates, self)
        dlg.setWindowTitle("Quick open")
        dlg.move(self.mapToGlobal(self.rect().center()) - dlg.rect().center())
        dlg.exec()

    def _about(self) -> None:
        QMessageBox.about(
            self, "ve-ide",
            "<h3>ve-ide — Visible Ephemeris IDE</h3>"
            "<p>VS Code-style Qt shell for the Visible Ephemeris project.</p>"
            "<p>Built on PyQt6 / Qt 6. See <code>docs/dashboards.md</code> for the full guide.</p>"
        )

    def closeEvent(self, event) -> None:
        dirty = [self.editor_tabs.widget(i) for i in range(self.editor_tabs.count())
                 if isinstance(self.editor_tabs.widget(i), EditorTab) and self.editor_tabs.widget(i).is_dirty()]
        if dirty:
            r = QMessageBox.question(self, "Unsaved changes",
                                     f"{len(dirty)} tab(s) have unsaved changes. Save all before quitting?",
                                     QMessageBox.StandardButton.SaveAll | QMessageBox.StandardButton.Discard | QMessageBox.StandardButton.Cancel)
            if r == QMessageBox.StandardButton.Cancel:
                event.ignore(); return
            if r == QMessageBox.StandardButton.SaveAll:
                self.save_all()
        self._stop_process()
        # Ensure the satellite-feed client thread also stops.
        try:
            if self.sat_feed.client is not None:
                self.sat_feed.client.stop(); self.sat_feed.client.wait(1500)
        except Exception:
            pass
        super().closeEvent(event)


# ---------------------------------------------------------------------------
# Simple embedded terminal via QProcess + QPlainTextEdit
# ---------------------------------------------------------------------------

class _Terminal(QWidget):
    def __init__(self, cwd: str, parent=None):
        super().__init__(parent)
        v = QVBoxLayout(self); v.setContentsMargins(4, 4, 4, 4); v.setSpacing(4)
        self.view = QPlainTextEdit(); self.view.setReadOnly(True)
        self.view.setFont(monospace_font(10))
        self.view.setStyleSheet(f"background:{PALETTE.bg_editor}; color:{PALETTE.fg_default};")
        v.addWidget(self.view, 1)

        row = QHBoxLayout()
        self.prompt = QLabel(); self._set_prompt(cwd)
        row.addWidget(self.prompt)
        self.line = QLineEdit(); row.addWidget(self.line, 1)
        v.addLayout(row)
        self.line.returnPressed.connect(self._run)

        self._cwd = os.path.abspath(cwd)
        self._proc: Optional[QProcess] = None

    def _set_prompt(self, cwd: str) -> None:
        short = os.path.basename(cwd) or cwd
        self.prompt.setText(f"[{short}] $")
        self.prompt.setStyleSheet(f"color:{PALETTE.fg_ok}; padding: 0 6px;")

    def set_cwd(self, cwd: str) -> None:
        self._cwd = os.path.abspath(cwd)
        self._set_prompt(self._cwd)

    def _run(self) -> None:
        cmd = self.line.text().strip()
        if not cmd:
            return
        self.line.clear()
        self.view.appendPlainText(f"[{os.path.basename(self._cwd) or self._cwd}] $ {cmd}")
        if cmd.split()[0] == "cd":
            target = " ".join(cmd.split()[1:]) or os.path.expanduser("~")
            new = target if os.path.isabs(target) else os.path.join(self._cwd, target)
            if os.path.isdir(new):
                self.set_cwd(os.path.normpath(new))
            else:
                self.view.appendPlainText(f"cd: {target}: no such directory")
            return
        if cmd.strip() == "clear":
            self.view.clear(); return
        self._proc = QProcess(self)
        self._proc.setWorkingDirectory(self._cwd)
        self._proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self._proc.readyReadStandardOutput.connect(
            lambda: self.view.appendPlainText(bytes(self._proc.readAllStandardOutput()).decode("utf-8", errors="replace").rstrip())
        )
        self._proc.finished.connect(lambda code, _s: self.view.appendPlainText(f"[exit {code}]"))
        # Run through sh -c so pipes, redirects, quoting all work.
        self._proc.start("/bin/sh", ["-c", cmd])


# ---------------------------------------------------------------------------
# Utility bits
# ---------------------------------------------------------------------------

def _letter_icon(letter: str) -> QIcon:
    """Render a coloured letter as a QIcon — avoids shipping icon assets."""
    pix = QPixmap(24, 24)
    pix.fill(QColor(0, 0, 0, 0))
    from PyQt6.QtGui import QPainter, QPen
    painter = QPainter(pix)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setPen(QPen(QColor(PALETTE.fg_default)))
    f = QFont(); f.setBold(True); f.setPointSize(12)
    painter.setFont(f)
    painter.drawText(pix.rect(), Qt.AlignmentFlag.AlignCenter, letter)
    painter.end()
    return QIcon(pix)


def _welcome_html(root: str) -> str:
    return f"""
    <div style="font-family: sans-serif; color: {PALETTE.fg_default}; padding: 24px;">
      <h1 style="color:{PALETTE.fg_link}; margin-bottom: 4px;">ve-ide</h1>
      <div style="color:{PALETTE.fg_muted};">Visible Ephemeris IDE — VS Code-style Qt shell</div>
      <hr style="border: 0; border-top: 1px solid {PALETTE.border}; margin: 20px 0;">
      <div style="color:{PALETTE.fg_muted}; margin-bottom: 6px;">Project root</div>
      <pre style="color:{PALETTE.fg_default}; background:{PALETTE.bg_sidebar}; padding: 10px;">{root}</pre>
      <h3 style="margin-top: 24px;">Get started</h3>
      <ul style="line-height: 1.7;">
        <li><b>Ctrl+P</b> — quick open a file by name</li>
        <li><b>Ctrl+Shift+P</b> — command palette (all actions)</li>
        <li><b>Ctrl+Shift+F</b> — search across files</li>
        <li><b>F5</b> — run the Python tracker (output streams to the Output tab)</li>
        <li><b>Ctrl+Shift+B</b> — cmake build</li>
        <li><b>Ctrl+`</b> — focus the terminal</li>
      </ul>
      <h3 style="margin-top: 24px;">Dashboards at your fingertips</h3>
      <p>The <i>Run</i> activity in the left bar has one-click launchers for the
        <b>Orbital Architect</b> and <b>HPOP diagnostic panel</b>. The
        <b>Satellites</b> tab in the bottom panel connects to the tracker's
        physics stream so you can watch the fleet live inside the IDE.</p>
    </div>
    """


def main(argv: Optional[list[str]] = None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="VS Code-style Qt IDE for Visible Ephemeris.")
    ap.add_argument("root", nargs="?", default=os.getcwd(), help="Project root to open (default: cwd).")
    args = ap.parse_args(argv[1:] if argv else None)

    app = QApplication(argv if argv is not None else sys.argv)
    apply_dark_theme(app)
    win = IDEWindow(root=args.root)
    win.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
