#!/usr/bin/env python3
"""Live orbit-determination panel (Qt).

Sibling of ``ve-hpop-panel``. Loads a TLE (from tle_cache/ or pasted),
takes a ground-station location (typed in or set via GPS/geoclue), lets
the user pick a Doppler source (hamlib rigctld or a raw numeric socket
stream), and drives the audited EKF in ``python_tracker/live_od_filter``
to improve the state vector in real time.

Two Doppler-source modes:

  * ``hamlib``: poll a running ``rigctld`` (default 127.0.0.1:4532) for
    the receiver's current tuned frequency at ~1 Hz. The measured
    Doppler shift is (rx_freq - f_transmit); the panel subtracts the
    frozen transmit reference and feeds the shift as observed frequency
    to the filter (i.e. the receiver is assumed to be tuned to the
    beacon carrier and the DSP reports the demodulated offset).
  * ``socket``: connect to a TCP endpoint that publishes one Doppler
    shift per line, in Hz (e.g. ``+1234.5``). Simplest possible ingest.

Both sources feed the same thread-safe observation queue; the filter
runs on the Qt main thread via a QTimer that drains that queue.

Launch:
    python3 -m qt_dashboards.live_od_panel
"""
from __future__ import annotations

import glob
import os
import queue
import socket
import sys
import threading
import time
from datetime import datetime, timezone
from typing import Optional

import numpy as np

from PyQt6.QtCore import Qt, QThread, QTimer, pyqtSignal
from PyQt6.QtGui import QAction
from PyQt6.QtWidgets import (
    QApplication,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QRadioButton,
    QSpinBox,
    QStatusBar,
    QVBoxLayout,
    QWidget,
)

import pyqtgraph as pg

# Local imports -- add parent so `live_od_filter` resolves either when
# run as `python3 -m qt_dashboards.live_od_panel` or directly.
_HERE = os.path.dirname(os.path.abspath(__file__))
_TRACKER_DIR = os.path.dirname(_HERE)
if _TRACKER_DIR not in sys.path:
    sys.path.insert(0, _TRACKER_DIR)
from live_od_filter import (  # noqa: E402
    LiveOdConfig, LiveOdFilter, LiveOdSnapshot, tle_epoch_state,
)


# ---------------------------------------------------------------------------
# Doppler source threads
# ---------------------------------------------------------------------------
class HamlibPoller(threading.Thread):
    """Poll rigctld for the receiver's tuned frequency at the given cadence.

    Emits (timestamp_utc, doppler_shift_hz) into an out-queue. The Doppler
    shift is defined as (rig frequency - f_transmit_reference); the caller
    passes f_transmit_reference so the filter sees f_transmit + shift and
    the arithmetic matches predict_doppler's convention.
    """

    def __init__(self, host: str, port: int, f_transmit_hz: float,
                 out_q: "queue.Queue", period_sec: float = 1.0):
        super().__init__(daemon=True)
        self.host = host
        self.port = port
        self.f_transmit = f_transmit_hz
        self.out_q = out_q
        self.period = period_sec
        self.stop_evt = threading.Event()
        self.status_msg: str = "starting"

    def run(self):
        try:
            with socket.create_connection((self.host, self.port), timeout=3.0) as s:
                s.settimeout(2.0)
                self.status_msg = f"connected {self.host}:{self.port}"
                buf = b""
                while not self.stop_evt.is_set():
                    try:
                        s.sendall(b"f\n")   # rigctld: "f" -> frequency
                    except OSError as e:
                        self.status_msg = f"send failed: {e}"; return
                    try:
                        chunk = s.recv(256)
                    except socket.timeout:
                        chunk = b""
                    if chunk:
                        buf += chunk
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            self._handle_line(line.strip().decode(errors="replace"))
                    self.stop_evt.wait(self.period)
        except OSError as e:
            self.status_msg = f"connect failed: {e}"

    def _handle_line(self, line: str):
        # rigctld returns just the integer frequency (Hz) on success, or
        # "RPRT <errno>" on failure.
        if not line or line.startswith("RPRT"):
            return
        try:
            freq = float(line)
        except ValueError:
            return
        shift = freq - self.f_transmit
        # rigctld usually reports the tuned frequency; the "received"
        # frequency the DSP is centred on. We feed f_transmit + shift to
        # the filter (matches the reference-frame convention).
        f_meas = self.f_transmit + shift
        self.out_q.put((datetime.now(tz=timezone.utc), f_meas))

    def stop(self):
        self.stop_evt.set()


class RawSocketReader(threading.Thread):
    """Read whitespace-delimited Doppler-shift Hz values from a TCP socket.

    Line protocol: each line is a signed float (Hz) representing the
    measured Doppler shift; the filter is fed f_transmit + shift.
    """

    def __init__(self, host: str, port: int, f_transmit_hz: float,
                 out_q: "queue.Queue"):
        super().__init__(daemon=True)
        self.host = host
        self.port = port
        self.f_transmit = f_transmit_hz
        self.out_q = out_q
        self.stop_evt = threading.Event()
        self.status_msg: str = "starting"

    def run(self):
        try:
            with socket.create_connection((self.host, self.port), timeout=3.0) as s:
                s.settimeout(1.0)
                self.status_msg = f"connected {self.host}:{self.port}"
                buf = b""
                while not self.stop_evt.is_set():
                    try:
                        chunk = s.recv(4096)
                    except socket.timeout:
                        continue
                    if not chunk:
                        self.status_msg = "peer closed"; return
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        try:
                            shift = float(line.strip())
                        except (ValueError, UnicodeDecodeError):
                            continue
                        self.out_q.put(
                            (datetime.now(tz=timezone.utc),
                             self.f_transmit + shift))
        except OSError as e:
            self.status_msg = f"connect failed: {e}"

    def stop(self):
        self.stop_evt.set()


# ---------------------------------------------------------------------------
# Qt window
# ---------------------------------------------------------------------------
class LiveOdWindow(QMainWindow):
    HISTORY = 600

    def __init__(self):
        super().__init__()
        self.setWindowTitle("Live OD — Doppler-driven state-vector improvement")
        self.resize(1500, 950)

        self._filter: Optional[LiveOdFilter] = None
        self._source_thread: Optional[threading.Thread] = None
        self._obs_q: "queue.Queue" = queue.Queue()
        self._history_t: list[float] = []
        self._history_nis: list[float] = []
        self._history_sigma_pos: list[float] = []

        self._build_ui()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._drain_observations)
        self._timer.start(200)

    # ---- UI ---------------------------------------------------------------

    def _build_ui(self):
        pg.setConfigOption("background", "#1e1e1e")
        pg.setConfigOption("foreground", "#d4d4d4")
        root = QWidget()
        self.setCentralWidget(root)
        outer = QHBoxLayout(root)

        # ----- LEFT column: configuration -----
        left = QVBoxLayout()
        outer.addLayout(left, 1)

        # TLE picker
        gb_tle = QGroupBox("Satellite TLE")
        v_tle = QVBoxLayout(gb_tle)
        h1 = QHBoxLayout()
        h1.addWidget(QLabel("From tle_cache/"))
        self.tle_dropdown = QComboBox()
        self.tle_dropdown.setMinimumWidth(220)
        self._populate_tle_dropdown()
        self.tle_dropdown.currentIndexChanged.connect(self._on_tle_dropdown)
        h1.addWidget(self.tle_dropdown, 1)
        self.btn_refresh_tle = QPushButton("Refresh")
        self.btn_refresh_tle.clicked.connect(self._populate_tle_dropdown)
        h1.addWidget(self.btn_refresh_tle)
        v_tle.addLayout(h1)
        v_tle.addWidget(QLabel("...or paste a 3-line TLE below:"))
        self.tle_paste = QPlainTextEdit()
        self.tle_paste.setPlaceholderText("NAME\n1 NNNNNU ...\n2 NNNNN ...")
        self.tle_paste.setMaximumHeight(80)
        v_tle.addWidget(self.tle_paste)
        left.addWidget(gb_tle)

        # Station location
        gb_stn = QGroupBox("Ground station (WGS-84)")
        g = QGridLayout(gb_stn)
        self.lat_edit = QDoubleSpinBox(); self.lat_edit.setRange(-90.0, 90.0)
        self.lat_edit.setDecimals(6); self.lat_edit.setValue(39.1434); self.lat_edit.setSuffix(" deg")
        self.lon_edit = QDoubleSpinBox(); self.lon_edit.setRange(-180.0, 180.0)
        self.lon_edit.setDecimals(6); self.lon_edit.setValue(-77.2014); self.lon_edit.setSuffix(" deg")
        self.alt_edit = QDoubleSpinBox(); self.alt_edit.setRange(-0.5, 10.0)
        self.alt_edit.setDecimals(4); self.alt_edit.setValue(0.153); self.alt_edit.setSuffix(" km")
        g.addWidget(QLabel("Latitude"), 0, 0); g.addWidget(self.lat_edit, 0, 1)
        g.addWidget(QLabel("Longitude"), 1, 0); g.addWidget(self.lon_edit, 1, 1)
        g.addWidget(QLabel("Altitude"), 2, 0); g.addWidget(self.alt_edit, 2, 1)
        left.addWidget(gb_stn)

        # Beacon
        gb_bx = QGroupBox("Beacon")
        b = QGridLayout(gb_bx)
        self.freq_edit = QDoubleSpinBox(); self.freq_edit.setRange(1e6, 100e9)
        self.freq_edit.setDecimals(0); self.freq_edit.setSingleStep(1e6)
        self.freq_edit.setValue(2.4e9); self.freq_edit.setSuffix(" Hz")
        b.addWidget(QLabel("Transmit freq (f_T)"), 0, 0); b.addWidget(self.freq_edit, 0, 1)
        self.sigma_edit = QDoubleSpinBox(); self.sigma_edit.setRange(0.1, 1000.0)
        self.sigma_edit.setValue(5.0); self.sigma_edit.setSuffix(" Hz")
        b.addWidget(QLabel("Meas noise (1-σ)"), 1, 0); b.addWidget(self.sigma_edit, 1, 1)
        left.addWidget(gb_bx)

        # Doppler source
        gb_src = QGroupBox("Doppler source")
        s = QVBoxLayout(gb_src)
        self.rb_hamlib = QRadioButton("Hamlib (rigctld)")
        self.rb_socket = QRadioButton("Raw Doppler-Hz socket stream")
        self.rb_hamlib.setChecked(True)
        s.addWidget(self.rb_hamlib)
        s.addWidget(self.rb_socket)
        h_src = QHBoxLayout()
        h_src.addWidget(QLabel("Host"))
        self.src_host = QLineEdit("127.0.0.1")
        h_src.addWidget(self.src_host)
        h_src.addWidget(QLabel("Port"))
        self.src_port = QSpinBox(); self.src_port.setRange(1, 65535); self.src_port.setValue(4532)
        h_src.addWidget(self.src_port)
        s.addLayout(h_src)
        self.rb_hamlib.toggled.connect(
            lambda on: self.src_port.setValue(4532 if on else 5555))
        left.addWidget(gb_src)

        # Start/Stop
        h_ctl = QHBoxLayout()
        self.btn_start = QPushButton("Start")
        self.btn_start.clicked.connect(self._start)
        h_ctl.addWidget(self.btn_start)
        self.btn_stop = QPushButton("Stop")
        self.btn_stop.clicked.connect(self._stop)
        self.btn_stop.setEnabled(False)
        h_ctl.addWidget(self.btn_stop)
        left.addLayout(h_ctl)
        left.addStretch(1)

        # ----- RIGHT column: readouts + plots -----
        right = QVBoxLayout()
        outer.addLayout(right, 2)

        gb_state = QGroupBox("Current estimate")
        gs = QGridLayout(gb_state)
        self.lbl_epoch = QLabel("—")
        self.lbl_r = QLabel("—"); self.lbl_v = QLabel("—")
        self.lbl_bc = QLabel("—"); self.lbl_bd = QLabel("—")
        self.lbl_sigma_pos = QLabel("—"); self.lbl_sigma_vel = QLabel("—")
        self.lbl_updates = QLabel("0")
        for lbl in (self.lbl_epoch, self.lbl_r, self.lbl_v, self.lbl_bc,
                    self.lbl_bd, self.lbl_sigma_pos, self.lbl_sigma_vel,
                    self.lbl_updates):
            lbl.setStyleSheet("font-family: monospace;")
        gs.addWidget(QLabel("Latest measurement"), 0, 0); gs.addWidget(self.lbl_epoch, 0, 1, 1, 3)
        gs.addWidget(QLabel("r [km]"), 1, 0); gs.addWidget(self.lbl_r, 1, 1, 1, 3)
        gs.addWidget(QLabel("v [km/s]"), 2, 0); gs.addWidget(self.lbl_v, 2, 1, 1, 3)
        gs.addWidget(QLabel("bc [Hz]"), 3, 0); gs.addWidget(self.lbl_bc, 3, 1)
        gs.addWidget(QLabel("bd [Hz/s]"), 3, 2); gs.addWidget(self.lbl_bd, 3, 3)
        gs.addWidget(QLabel("σ_pos [km]"), 4, 0); gs.addWidget(self.lbl_sigma_pos, 4, 1)
        gs.addWidget(QLabel("σ_vel [km/s]"), 4, 2); gs.addWidget(self.lbl_sigma_vel, 4, 3)
        gs.addWidget(QLabel("# updates"), 5, 0); gs.addWidget(self.lbl_updates, 5, 1)
        right.addWidget(gb_state)

        gb_plots = QGroupBox("Live diagnostics")
        pv = QVBoxLayout(gb_plots)
        self.nis_plot = pg.PlotWidget(title="NIS (target ~1 for well-tuned NY=1)")
        self.nis_plot.setLabel("bottom", "t since start (s)")
        self.nis_plot.setLabel("left", "NIS")
        self.nis_curve = self.nis_plot.plot(pen=pg.mkPen("#4ec9b0", width=2))
        pv.addWidget(self.nis_plot)
        self.sigma_plot = pg.PlotWidget(title="Position 1-σ (km)")
        self.sigma_plot.setLabel("bottom", "t since start (s)")
        self.sigma_plot.setLabel("left", "σ_pos (km)")
        self.sigma_curve = self.sigma_plot.plot(pen=pg.mkPen("#dcdcaa", width=2))
        pv.addWidget(self.sigma_plot)
        right.addWidget(gb_plots, 1)

        self.setStatusBar(QStatusBar())
        self.statusBar().showMessage("Idle")

    # ---- TLE helpers ------------------------------------------------------

    def _populate_tle_dropdown(self):
        self.tle_dropdown.blockSignals(True)
        self.tle_dropdown.clear()
        self.tle_dropdown.addItem("(select from tle_cache/)")
        cache_dir = os.path.join(_TRACKER_DIR, "..", "tle_cache")
        cache_dir = os.path.abspath(cache_dir)
        if os.path.isdir(cache_dir):
            for path in sorted(glob.glob(os.path.join(cache_dir, "*.txt"))):
                self._add_tles_from_file(path)
        self.tle_dropdown.blockSignals(False)

    def _add_tles_from_file(self, path: str):
        try:
            with open(path, "r", errors="replace") as f:
                lines = [ln.rstrip("\n") for ln in f]
        except OSError:
            return
        i = 0
        base = os.path.basename(path)
        while i + 2 < len(lines):
            if lines[i + 1].startswith("1 ") and lines[i + 2].startswith("2 "):
                name = lines[i].strip()
                self.tle_dropdown.addItem(f"{base} :: {name}",
                                          (name, lines[i + 1], lines[i + 2]))
                i += 3
            else:
                i += 1

    def _on_tle_dropdown(self, idx: int):
        data = self.tle_dropdown.itemData(idx)
        if data:
            name, l1, l2 = data
            self.tle_paste.setPlainText(f"{name}\n{l1}\n{l2}")

    def _current_tle(self) -> Optional[tuple[str, str, str]]:
        text = self.tle_paste.toPlainText().strip()
        if not text:
            return None
        lines = [ln for ln in text.splitlines() if ln.strip()]
        if len(lines) < 3:
            return None
        return (lines[0].strip(), lines[1].strip(), lines[2].strip())

    # ---- Start / Stop -----------------------------------------------------

    def _start(self):
        tle = self._current_tle()
        if tle is None:
            QMessageBox.warning(self, "TLE missing",
                                "Select or paste a 3-line TLE first.")
            return
        try:
            name, l1, l2 = tle
            epoch_utc, jd, r, v = tle_epoch_state(name, l1, l2)
        except Exception as e:
            QMessageBox.critical(self, "TLE parse failed", str(e))
            return
        # Filter config
        f_transmit = float(self.freq_edit.value())
        cfg = LiveOdConfig(
            station_lat_deg=self.lat_edit.value(),
            station_lon_deg=self.lon_edit.value(),
            station_alt_km=self.alt_edit.value(),
            f_transmit_hz=f_transmit,
            sigma_R_hz=self.sigma_edit.value(),
        )
        x0 = np.zeros(8)
        x0[0:3] = r; x0[3:6] = v
        self._filter = LiveOdFilter(cfg, epoch_utc, jd, x0)
        self._t0 = time.time()
        self._history_t.clear()
        self._history_nis.clear()
        self._history_sigma_pos.clear()
        # Start Doppler source thread
        host = self.src_host.text().strip() or "127.0.0.1"
        port = int(self.src_port.value())
        if self.rb_hamlib.isChecked():
            self._source_thread = HamlibPoller(host, port, f_transmit, self._obs_q)
        else:
            self._source_thread = RawSocketReader(host, port, f_transmit, self._obs_q)
        self._source_thread.start()
        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.statusBar().showMessage(
            f"Filter armed. Source: {type(self._source_thread).__name__} "
            f"→ {host}:{port}. Epoch: {epoch_utc.isoformat()}")

    def _stop(self):
        if self._source_thread is not None:
            self._source_thread.stop()
            self._source_thread.join(timeout=2.0)
            self._source_thread = None
        self._filter = None
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self.statusBar().showMessage("Stopped")

    # ---- Event loop drain ------------------------------------------------

    def _drain_observations(self):
        if self._filter is None:
            if self._source_thread is not None:
                self.statusBar().showMessage(self._source_thread.status_msg)
            return
        pulled = 0
        latest: Optional[LiveOdSnapshot] = None
        while True:
            try:
                t_utc, f_hz = self._obs_q.get_nowait()
            except queue.Empty:
                break
            try:
                latest = self._filter.update(t_utc, f_hz)
                pulled += 1
            except Exception as e:
                self.statusBar().showMessage(f"filter error: {e}")
                break
        if latest is not None:
            self._render(latest)

    def _render(self, snap: LiveOdSnapshot):
        self.lbl_epoch.setText(snap.t_utc.isoformat(timespec="seconds"))
        r = snap.r_km; v = snap.v_km_s
        self.lbl_r.setText(f"{r[0]:+.3f}  {r[1]:+.3f}  {r[2]:+.3f}")
        self.lbl_v.setText(f"{v[0]:+.6f}  {v[1]:+.6f}  {v[2]:+.6f}")
        self.lbl_bc.setText(f"{snap.bc_hz:+.3f}")
        self.lbl_bd.setText(f"{snap.bd_hz_s:+.6f}")
        self.lbl_sigma_pos.setText(f"{snap.sigma_pos_km:.3f}")
        self.lbl_sigma_vel.setText(f"{snap.sigma_vel_km_s:.6f}")
        self.lbl_updates.setText(str(snap.n_updates))
        t = time.time() - self._t0
        self._history_t.append(t)
        self._history_nis.append(snap.nis)
        self._history_sigma_pos.append(snap.sigma_pos_km)
        if len(self._history_t) > self.HISTORY:
            del self._history_t[:len(self._history_t) - self.HISTORY]
            del self._history_nis[:len(self._history_nis) - self.HISTORY]
            del self._history_sigma_pos[:len(self._history_sigma_pos) - self.HISTORY]
        self.nis_curve.setData(self._history_t, self._history_nis)
        self.sigma_curve.setData(self._history_t, self._history_sigma_pos)


def main(argv: Optional[list[str]] = None) -> int:
    app = QApplication(argv or sys.argv)
    w = LiveOdWindow()
    w.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
