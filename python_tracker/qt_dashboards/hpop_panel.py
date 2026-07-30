"""HPOP / physics-stream diagnostic Qt dashboard.

Connects to the running tracker's PhysicsServer TCP stream (default host
``127.0.0.1``, port ``12346``) and renders live diagnostics:

    * Header readout (timestamp, observer, visible-count)
    * Sortable satellite table for the current frame
    * pyqtgraph strip charts for the selected satellite:
        - elevation (deg)
        - range (km)
        - range-rate (km/s)
      plus a chart of the fleet's visible-count over time.
    * Raw-frame log (bottom dock) for troubleshooting.

The client is intentionally reconnect-friendly: if the stream goes away it keeps
trying every few seconds while surfacing the connection state in the status bar.
"""
from __future__ import annotations

import re
import socket
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Deque, Dict, List, Optional

from PyQt6.QtCore import Qt, QCoreApplication, QThread, pyqtSignal
from PyQt6.QtGui import QAction, QKeySequence
from PyQt6.QtWidgets import (
    QApplication,
    QComboBox,
    QDockWidget,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QSplitter,
    QStatusBar,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

import pyqtgraph as pg

from .theme import PALETTE, LogPane, StatusBar, apply_dark_theme, monospace_font, ui_font


FRAME_MARK = "---END_FRAME---"


@dataclass
class SatRow:
    name: str
    az: float
    el: float
    range_km: float
    range_rate: float
    vis: str
    next_event: str
    norad: int
    lat: float
    lon: float
    apogee: float
    flare: int


@dataclass
class Frame:
    timestamp: str = ""
    observer: str = ""
    visible_count: int = 0
    total_shown: int = 0
    sats: List[SatRow] = field(default_factory=list)
    raw: str = ""


_HEADER_RE = re.compile(
    r"^\s*(?P<name>.{1,15}?)\s+"
    r"(?P<az>-?\d+\.\d+)\s+"
    r"(?P<el>-?\d+\.\d+)\s+"
    r"(?P<range>-?\d+\.\d+)\s+"
    r"(?P<rr>-?\d+\.\d+)\s+"
    r"(?P<vis>\S+)\s+"
    r"(?P<next>\S+)\s+"
    r"(?P<norad>\d+)\s+"
    r"(?P<lat>-?\d+\.\d+)\s+"
    r"(?P<lon>-?\d+\.\d+)\s+"
    r"(?P<apo>-?\d+\.\d+)\s+"
    r"(?P<flare>-?\d+)\s*$"
)


def parse_frame(raw: str) -> Frame:
    """Parse one PhysicsServer text frame into a Frame dataclass."""
    frame = Frame(raw=raw)
    for line in raw.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.endswith("LOC"):
            frame.timestamp = stripped[:-3].strip()
            continue
        if stripped.startswith("OBS:"):
            frame.observer = stripped
            m = re.search(r"SHOWN:\s*(\d+)", stripped)
            if m:
                frame.total_shown = int(m.group(1))
            continue
        if stripped.startswith("NAME") or stripped.startswith("-"):
            continue
        m = _HEADER_RE.match(line)
        if not m:
            continue
        try:
            frame.sats.append(SatRow(
                name=m.group("name").strip(),
                az=float(m.group("az")),
                el=float(m.group("el")),
                range_km=float(m.group("range")),
                range_rate=float(m.group("rr")),
                vis=m.group("vis"),
                next_event=m.group("next"),
                norad=int(m.group("norad")),
                lat=float(m.group("lat")),
                lon=float(m.group("lon")),
                apogee=float(m.group("apo")),
                flare=int(m.group("flare")),
            ))
        except ValueError:
            continue
    frame.visible_count = sum(1 for s in frame.sats if s.el >= 0)
    return frame


class PhysicsClient(QThread):
    """Background socket reader that emits parsed frames as they arrive."""

    frame_received = pyqtSignal(object)  # Frame
    status_changed = pyqtSignal(bool, str)  # (connected, message)
    raw_chunk = pyqtSignal(str)

    def __init__(self, host: str, port: int, parent=None):
        super().__init__(parent)
        self.host = host
        self.port = port
        self._stop = False
        self._buf = ""
        # Qt 6.11+ turns "QThread destroyed while still running" into a fatal
        # abort. Guarantee the socket loop is torn down before the QApplication
        # goes away by hooking aboutToQuit.
        app = QCoreApplication.instance()
        if app is not None:
            app.aboutToQuit.connect(self._graceful_stop)

    def _graceful_stop(self) -> None:
        self.stop()
        # Give the loop up to 1 s to fall out of recv/sleep.
        self.wait(1000)

    def stop(self) -> None:
        self._stop = True

    def _connect(self) -> Optional[socket.socket]:
        try:
            s = socket.create_connection((self.host, self.port), timeout=3.0)
            s.settimeout(1.0)
            self.status_changed.emit(True, f"connected {self.host}:{self.port}")
            return s
        except OSError as e:
            self.status_changed.emit(False, f"{self.host}:{self.port} — {e}")
            return None

    def run(self) -> None:
        while not self._stop:
            sock = self._connect()
            if sock is None:
                for _ in range(30):
                    if self._stop:
                        return
                    self.msleep(100)
                continue
            self._buf = ""
            try:
                while not self._stop:
                    try:
                        chunk = sock.recv(8192)
                    except socket.timeout:
                        continue
                    if not chunk:
                        raise ConnectionResetError("EOF")
                    text = chunk.decode("utf-8", errors="replace")
                    self.raw_chunk.emit(text)
                    self._buf += text
                    while FRAME_MARK in self._buf:
                        raw, self._buf = self._buf.split(FRAME_MARK, 1)
                        try:
                            self.frame_received.emit(parse_frame(raw))
                        except Exception as e:  # noqa: BLE001
                            self.status_changed.emit(True, f"parse error: {e}")
            except OSError as e:
                self.status_changed.emit(False, f"disconnected: {e}")
            finally:
                try:
                    sock.close()
                except OSError:
                    pass


class HPOPWindow(QMainWindow):
    HISTORY = 600  # samples per strip chart

    def __init__(self, host: str = "127.0.0.1", port: int = 12346):
        super().__init__()
        self.setWindowTitle("HPOP diagnostic — Visible Ephemeris")
        self.resize(1400, 900)

        self._host = host
        self._port = port
        self._current: Optional[Frame] = None
        self._selected_norad: Optional[int] = None

        self._el_hist: Dict[int, Deque[tuple[float, float]]] = {}
        self._rng_hist: Dict[int, Deque[tuple[float, float]]] = {}
        self._rr_hist: Dict[int, Deque[tuple[float, float]]] = {}
        self._count_hist: Deque[tuple[float, int]] = deque(maxlen=self.HISTORY)

        self._t0 = time.time()

        self._build_ui()
        self._start_client()

    def _build_ui(self) -> None:
        pg.setConfigOption("background", PALETTE.bg_editor)
        pg.setConfigOption("foreground", PALETTE.fg_default)

        root = QWidget()
        self.setCentralWidget(root)
        v = QVBoxLayout(root)
        v.setContentsMargins(6, 6, 6, 6)
        v.setSpacing(6)

        # --- Header bar -----------------------------------------------------
        hdr = QHBoxLayout()
        title = QLabel("HPOP / Physics stream diagnostic")
        f = ui_font(12); f.setBold(True); title.setFont(f)
        hdr.addWidget(title)
        hdr.addStretch(1)
        hdr.addWidget(QLabel("Host"))
        self.host_edit = QComboBox()
        self.host_edit.setEditable(True)
        self.host_edit.addItems(["127.0.0.1", "localhost"])
        self.host_edit.setEditText(self._host)
        hdr.addWidget(self.host_edit)
        hdr.addWidget(QLabel("Port"))
        self.port_spin = QSpinBox(); self.port_spin.setRange(1, 65535); self.port_spin.setValue(self._port)
        hdr.addWidget(self.port_spin)
        self.reconnect_btn = QPushButton("Reconnect")
        self.reconnect_btn.clicked.connect(self._reconnect)
        hdr.addWidget(self.reconnect_btn)
        v.addLayout(hdr)

        # Header readout
        readout = QGroupBox("Current frame")
        r = QGridLayout(readout)
        self.lbl_time = QLabel("—"); self.lbl_obs = QLabel("—"); self.lbl_count = QLabel("—"); self.lbl_selected = QLabel("(none)")
        for lbl in (self.lbl_time, self.lbl_obs, self.lbl_count, self.lbl_selected):
            lbl.setFont(monospace_font(11))
        r.addWidget(QLabel("Timestamp"), 0, 0); r.addWidget(self.lbl_time, 0, 1, 1, 3)
        r.addWidget(QLabel("Observer"), 1, 0); r.addWidget(self.lbl_obs, 1, 1, 1, 3)
        r.addWidget(QLabel("Visible / shown"), 2, 0); r.addWidget(self.lbl_count, 2, 1)
        r.addWidget(QLabel("Selected"), 2, 2); r.addWidget(self.lbl_selected, 2, 3)
        v.addWidget(readout)

        # --- Middle: table left, plots right --------------------------------
        splitter = QSplitter(Qt.Orientation.Horizontal)
        v.addWidget(splitter, 1)

        # Table
        self.table = QTableWidget()
        self.table.setColumnCount(6)
        self.table.setHorizontalHeaderLabels(["Name", "NORAD", "El°", "Range km", "Range-rate", "Vis"])
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSortingEnabled(True)
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.setFont(monospace_font(10))
        self.table.itemSelectionChanged.connect(self._on_row_selected)
        splitter.addWidget(self.table)

        # Plots
        plot_holder = QWidget()
        pv = QVBoxLayout(plot_holder)
        pv.setContentsMargins(0, 0, 0, 0)
        pv.setSpacing(4)

        self.plot_el = pg.PlotWidget(title="Elevation (deg)")
        self.plot_rng = pg.PlotWidget(title="Range (km)")
        self.plot_rr = pg.PlotWidget(title="Range rate (km/s)")
        self.plot_count = pg.PlotWidget(title="Visible-count over time")
        for p in (self.plot_el, self.plot_rng, self.plot_rr, self.plot_count):
            p.showGrid(x=True, y=True, alpha=0.2)
            p.getAxis("bottom").setLabel("t (s)")
            pv.addWidget(p)
        self._curve_el = self.plot_el.plot(pen=pg.mkPen(PALETTE.fg_ok, width=2))
        self._curve_rng = self.plot_rng.plot(pen=pg.mkPen(PALETTE.fg_link, width=2))
        self._curve_rr = self.plot_rr.plot(pen=pg.mkPen(PALETTE.syn_number, width=2))
        self._curve_cnt = self.plot_count.plot(pen=pg.mkPen(PALETTE.accent, width=2))

        splitter.addWidget(plot_holder)
        splitter.setStretchFactor(0, 2)
        splitter.setStretchFactor(1, 3)

        # --- Dock: raw log --------------------------------------------------
        self.log = LogPane()
        dock = QDockWidget("Raw stream", self)
        dock.setAllowedAreas(Qt.DockWidgetArea.BottomDockWidgetArea | Qt.DockWidgetArea.TopDockWidgetArea)
        dock.setWidget(self.log)
        self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, dock)
        dock.hide()  # hidden by default; View menu / F4 toggles it
        self._raw_dock = dock

        toggle = QAction("Toggle raw stream", self)
        toggle.setShortcut(QKeySequence("F4"))
        toggle.triggered.connect(lambda: dock.setVisible(not dock.isVisible()))
        self.addAction(toggle)

        self.status = StatusBar()
        self.setStatusBar(self.status)
        self.status.showMessage("Starting…")

    # --- Client wiring ------------------------------------------------------
    def _start_client(self) -> None:
        self._client = PhysicsClient(self._host, self._port)
        self._client.frame_received.connect(self._on_frame)
        self._client.status_changed.connect(self._on_status)
        self._client.raw_chunk.connect(self._on_raw)
        self._client.start()

    def _reconnect(self) -> None:
        self._host = self.host_edit.currentText()
        self._port = self.port_spin.value()
        try:
            self._client.stop()
            self._client.wait(1500)
        except Exception:
            pass
        self._start_client()

    def _on_status(self, connected: bool, msg: str) -> None:
        self.status.set_connection(connected, msg)
        (self.log.ok if connected else self.log.warn)(msg)

    def _on_raw(self, chunk: str) -> None:
        # Only log tail chunks to keep the pane cheap.
        if self._raw_dock.isVisible():
            self.log.info(chunk.rstrip("\n"))

    def _on_frame(self, frame: Frame) -> None:
        self._current = frame
        self.lbl_time.setText(frame.timestamp or "—")
        self.lbl_obs.setText(frame.observer or "—")
        self.lbl_count.setText(f"{frame.visible_count} / {frame.total_shown or len(frame.sats)}")

        # History bookkeeping
        t = time.time() - self._t0
        self._count_hist.append((t, frame.visible_count))
        for s in frame.sats:
            self._el_hist.setdefault(s.norad, deque(maxlen=self.HISTORY)).append((t, s.el))
            self._rng_hist.setdefault(s.norad, deque(maxlen=self.HISTORY)).append((t, s.range_km))
            self._rr_hist.setdefault(s.norad, deque(maxlen=self.HISTORY)).append((t, s.range_rate))

        # Table
        self.table.setSortingEnabled(False)
        prev_sel = self._selected_norad
        self.table.setRowCount(len(frame.sats))
        for row, s in enumerate(frame.sats):
            for col, val in enumerate((
                s.name, s.norad, f"{s.el:.1f}", f"{s.range_km:.1f}", f"{s.range_rate:.3f}", s.vis
            )):
                item = QTableWidgetItem(str(val))
                if col in (1, 2, 3, 4):
                    item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                item.setData(Qt.ItemDataRole.UserRole, s.norad)
                self.table.setItem(row, col, item)
        self.table.setSortingEnabled(True)

        # Restore selection by NORAD
        if prev_sel is not None:
            for row in range(self.table.rowCount()):
                item = self.table.item(row, 1)
                if item and int(item.text()) == prev_sel:
                    self.table.selectRow(row)
                    break

        # Plots
        cnt = list(self._count_hist)
        self._curve_cnt.setData([p[0] for p in cnt], [p[1] for p in cnt])
        self._update_selected_plots()

    def _on_row_selected(self) -> None:
        rows = self.table.selectionModel().selectedRows()
        if not rows:
            return
        norad_item = self.table.item(rows[0].row(), 1)
        if not norad_item:
            return
        try:
            self._selected_norad = int(norad_item.text())
        except ValueError:
            self._selected_norad = None
        name_item = self.table.item(rows[0].row(), 0)
        self.lbl_selected.setText(f"{name_item.text() if name_item else '?'} [{self._selected_norad}]")
        self._update_selected_plots()

    def _update_selected_plots(self) -> None:
        nid = self._selected_norad
        if nid is None:
            return
        for hist, curve in ((self._el_hist, self._curve_el),
                            (self._rng_hist, self._curve_rng),
                            (self._rr_hist, self._curve_rr)):
            pts = list(hist.get(nid, deque()))
            curve.setData([p[0] for p in pts], [p[1] for p in pts])

    def closeEvent(self, event) -> None:
        try:
            self._client.stop()
            self._client.wait(1500)
        except Exception:
            pass
        super().closeEvent(event)


def main(argv: Optional[list[str]] = None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Qt HPOP / physics-stream dashboard.")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=12346)
    args = ap.parse_args(argv[1:] if argv else None)

    app = QApplication(argv if argv is not None else sys.argv)
    apply_dark_theme(app)
    win = HPOPWindow(host=args.host, port=args.port)
    win.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
