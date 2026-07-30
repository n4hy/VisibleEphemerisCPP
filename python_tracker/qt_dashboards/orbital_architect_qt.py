"""Orbital Architect (Qt) — satellite selection GUI.

A drop-in ergonomic replacement for the ANSI ``orbital_architect.py`` terminal
tool. Loads every ``tle_cache/*.txt`` file into a searchable in-memory catalog,
lets the user filter/pick satellites via checkboxes, edits the observer / mode
config, and on Deploy writes both a group TLE file and ``config.yaml``.

Design notes:
    * Left pane: search box + filterable catalog table (Name / NORAD / Source),
      selection column with a checkbox that toggles fleet membership.
    * Right pane: current fleet with count and one-click removal.
    * Bottom: observer + display-mode config, deploy button, status log.
    * Everything is live — typing in the search box filters immediately with no
      "search" button, matching what users expect from a modern picker.

Usage::

    python -m python_tracker.qt_dashboards.orbital_architect_qt
    ve-orbital-architect-qt            # installed launcher

Reads and writes ``config.yaml`` in the current working directory (falls back
to the repo root) and ``tle_cache/*.txt`` the same way the CLI tool does.
"""
from __future__ import annotations

import glob
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from PyQt6.QtCore import QSortFilterProxyModel, Qt, pyqtSignal
from PyQt6.QtGui import QAction, QKeySequence, QStandardItem, QStandardItemModel
from PyQt6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSplitter,
    QTableView,
    QVBoxLayout,
    QWidget,
)

from .theme import PALETTE, LogPane, StatusBar, apply_dark_theme, monospace_font, ui_font


CACHE_DIR_DEFAULT = "tle_cache"
CONFIG_FILE_DEFAULT = "config.yaml"


@dataclass
class Satellite:
    name: str
    l1: str
    l2: str
    source: str
    id: int = 0


@dataclass
class Catalog:
    by_id: Dict[int, Satellite] = field(default_factory=dict)


def _resolve_paths() -> tuple[str, str]:
    """Return (cache_dir, config_path). Try CWD then repo root."""
    for cwd in (os.getcwd(), os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))):
        cache = os.path.join(cwd, CACHE_DIR_DEFAULT)
        cfg = os.path.join(cwd, CONFIG_FILE_DEFAULT)
        if os.path.isdir(cache):
            return cache, cfg
    return CACHE_DIR_DEFAULT, CONFIG_FILE_DEFAULT


def load_catalog(cache_dir: str) -> Catalog:
    cat = Catalog()
    files = sorted(glob.glob(os.path.join(cache_dir, "*.txt")))
    for path in files:
        name = os.path.basename(path)
        if name == "user_defined.txt":
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                lines = [ln.rstrip("\n") for ln in f]
        except OSError:
            continue
        i = 0
        while i + 2 < len(lines):
            l0, l1, l2 = lines[i].strip(), lines[i + 1].strip(), lines[i + 2].strip()
            if l1.startswith("1 ") and l2.startswith("2 "):
                try:
                    nid = int(l1[2:7])
                except ValueError:
                    nid = 0
                cat.by_id[nid] = Satellite(name=l0, l1=l1, l2=l2, source=name, id=nid)
                i += 3
                continue
            i += 1
    return cat


def load_config(config_path: str) -> Dict[str, object]:
    cfg: Dict[str, object] = {
        "lat": 0.0, "lon": 0.0, "alt": 0.0,
        "min_el": 0.0, "max_apo": -1.0,
        "group_selection": "active",
        "show_all_visible": False,
        "sat_selection": "",
    }
    if not os.path.exists(config_path):
        return cfg
    with open(config_path, "r", encoding="utf-8") as f:
        for line in f:
            if ":" not in line or line.strip().startswith("#"):
                continue
            k, v = line.split(":", 1)
            k, v = k.strip(), v.strip()
            if v.lower() == "true":
                cfg[k] = True
            elif v.lower() == "false":
                cfg[k] = False
            else:
                try:
                    cfg[k] = float(v)
                except ValueError:
                    cfg[k] = v.strip('"').strip("'")
    return cfg


def save_config(config_path: str, cfg: Dict[str, object]) -> None:
    with open(config_path, "w", encoding="utf-8") as f:
        for k, v in cfg.items():
            if isinstance(v, bool):
                f.write(f"{k}: {'true' if v else 'false'}\n")
            else:
                f.write(f"{k}: {v}\n")


def sanitise_group(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_")
    return s or "user_defined"


class CatalogModel(QStandardItemModel):
    HEADERS = ("Name", "NORAD", "Source")

    def __init__(self, cat: Catalog, selected_ids: set[int]):
        super().__init__()
        self._sats: List[Satellite] = sorted(cat.by_id.values(), key=lambda s: s.name.lower())
        self._selected = selected_ids
        self.setHorizontalHeaderLabels(self.HEADERS)
        for s in self._sats:
            row = [
                QStandardItem(s.name),
                QStandardItem(str(s.id)),
                QStandardItem(s.source),
            ]
            row[0].setCheckable(True)
            row[0].setCheckState(Qt.CheckState.Checked if s.id in self._selected else Qt.CheckState.Unchecked)
            for it in row:
                it.setEditable(False)
                it.setData(s, Qt.ItemDataRole.UserRole)
            self.appendRow(row)


class SearchProxy(QSortFilterProxyModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFilterCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.setFilterKeyColumn(-1)  # search all columns


class ArchitectWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Orbital Architect — Visible Ephemeris")
        self.resize(1200, 780)

        self.cache_dir, self.config_path = _resolve_paths()
        self.cfg = load_config(self.config_path)
        self.selected: Dict[int, Satellite] = {}

        self._build_ui()
        self._install_shortcuts()
        self._reload_catalog()

    def _build_ui(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        # --- Top toolbar row ------------------------------------------------
        toolbar_row = QHBoxLayout()
        toolbar_row.setContentsMargins(10, 8, 10, 8)
        title = QLabel("Orbital Architect")
        f = ui_font(12)
        f.setBold(True)
        title.setFont(f)
        title.setStyleSheet(f"color: {PALETTE.fg_default};")
        toolbar_row.addWidget(title)
        toolbar_row.addStretch(1)
        self.reload_btn = QPushButton("Reload cache")
        self.reload_btn.clicked.connect(self._reload_catalog)
        toolbar_row.addWidget(self.reload_btn)
        self.pick_dir_btn = QPushButton("Choose tle_cache…")
        self.pick_dir_btn.clicked.connect(self._pick_cache_dir)
        toolbar_row.addWidget(self.pick_dir_btn)
        outer.addLayout(toolbar_row)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        outer.addWidget(splitter, 1)

        # --- Left: catalog --------------------------------------------------
        left = QWidget()
        left_v = QVBoxLayout(left)
        left_v.setContentsMargins(10, 4, 6, 10)
        left_v.setSpacing(6)

        search_row = QHBoxLayout()
        search_row.addWidget(QLabel("Search"))
        self.search = QLineEdit()
        self.search.setPlaceholderText("Type name substring or NORAD id (regex ok)…")
        self.search.setClearButtonEnabled(True)
        search_row.addWidget(self.search, 1)
        left_v.addLayout(search_row)

        self.catalog_view = QTableView()
        self.catalog_view.setSelectionBehavior(QTableView.SelectionBehavior.SelectRows)
        self.catalog_view.setSelectionMode(QTableView.SelectionMode.ExtendedSelection)
        self.catalog_view.setAlternatingRowColors(True)
        self.catalog_view.setSortingEnabled(True)
        self.catalog_view.horizontalHeader().setStretchLastSection(True)
        self.catalog_view.setFont(monospace_font(10))
        left_v.addWidget(self.catalog_view, 1)

        catalog_btns = QHBoxLayout()
        self.add_selected_btn = QPushButton("Add highlighted →")
        self.add_selected_btn.clicked.connect(self._add_highlighted)
        catalog_btns.addWidget(self.add_selected_btn)
        self.add_all_visible_btn = QPushButton("Add all filtered")
        self.add_all_visible_btn.clicked.connect(self._add_all_filtered)
        catalog_btns.addWidget(self.add_all_visible_btn)
        catalog_btns.addStretch(1)
        self.catalog_count = QLabel("0 / 0")
        self.catalog_count.setStyleSheet(f"color: {PALETTE.fg_muted};")
        catalog_btns.addWidget(self.catalog_count)
        left_v.addLayout(catalog_btns)

        splitter.addWidget(left)

        # --- Right: fleet + config ------------------------------------------
        right = QWidget()
        right_v = QVBoxLayout(right)
        right_v.setContentsMargins(6, 4, 10, 10)
        right_v.setSpacing(8)

        fleet_group = QGroupBox("Selected fleet")
        fleet_v = QVBoxLayout(fleet_group)
        self.fleet_list = QListWidget()
        self.fleet_list.setFont(monospace_font(10))
        self.fleet_list.setSelectionMode(QListWidget.SelectionMode.ExtendedSelection)
        fleet_v.addWidget(self.fleet_list, 1)
        fleet_btns = QHBoxLayout()
        self.remove_btn = QPushButton("← Remove")
        self.remove_btn.clicked.connect(self._remove_from_fleet)
        fleet_btns.addWidget(self.remove_btn)
        self.clear_btn = QPushButton("Clear fleet")
        self.clear_btn.clicked.connect(self._clear_fleet)
        fleet_btns.addWidget(self.clear_btn)
        fleet_btns.addStretch(1)
        self.fleet_count = QLabel("0 selected")
        self.fleet_count.setStyleSheet(f"color: {PALETTE.fg_muted};")
        fleet_btns.addWidget(self.fleet_count)
        fleet_v.addLayout(fleet_btns)
        right_v.addWidget(fleet_group, 1)

        cfg_group = QGroupBox("Station & mode")
        form = QFormLayout(cfg_group)
        self.lat = QDoubleSpinBox(); self.lat.setRange(-90.0, 90.0); self.lat.setDecimals(6); self.lat.setValue(float(self.cfg.get("lat", 0.0) or 0.0))
        self.lon = QDoubleSpinBox(); self.lon.setRange(-180.0, 180.0); self.lon.setDecimals(6); self.lon.setValue(float(self.cfg.get("lon", 0.0) or 0.0))
        self.alt = QDoubleSpinBox(); self.alt.setRange(-1.0, 10.0); self.alt.setDecimals(3); self.alt.setSuffix(" km"); self.alt.setValue(float(self.cfg.get("alt", 0.0) or 0.0))
        self.min_el = QDoubleSpinBox(); self.min_el.setRange(-5.0, 89.0); self.min_el.setDecimals(1); self.min_el.setSuffix(" °"); self.min_el.setValue(float(self.cfg.get("min_el", 0.0) or 0.0))
        self.max_apo = QDoubleSpinBox(); self.max_apo.setRange(-1.0, 500000.0); self.max_apo.setDecimals(1); self.max_apo.setSuffix(" km"); self.max_apo.setValue(float(self.cfg.get("max_apo", -1.0) or -1.0))
        self.radio_mode = QCheckBox("Radio mode (show all sats, not just optically visible)")
        self.radio_mode.setChecked(bool(self.cfg.get("show_all_visible", False)))

        form.addRow("Latitude", self.lat)
        form.addRow("Longitude", self.lon)
        form.addRow("Altitude", self.alt)
        form.addRow("Min elevation", self.min_el)
        form.addRow("Max apogee (−1 = off)", self.max_apo)
        form.addRow("", self.radio_mode)

        right_v.addWidget(cfg_group)

        deploy_row = QHBoxLayout()
        deploy_row.addWidget(QLabel("Group name"))
        self.group_name = QLineEdit(str(self.cfg.get("group_selection", "user_defined") or "user_defined"))
        deploy_row.addWidget(self.group_name, 1)
        self.deploy_btn = QPushButton("Deploy mission ▶")
        self.deploy_btn.setStyleSheet(f"background: {PALETTE.accent}; color: white; font-weight: bold;")
        self.deploy_btn.clicked.connect(self._deploy)
        deploy_row.addWidget(self.deploy_btn)
        right_v.addLayout(deploy_row)

        splitter.addWidget(right)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 2)

        # --- Log at bottom --------------------------------------------------
        self.log = LogPane()
        self.log.setFixedHeight(120)
        outer.addWidget(self.log)

        self.status = StatusBar()
        self.setStatusBar(self.status)
        self.status.showMessage(f"Cache: {self.cache_dir}   Config: {self.config_path}")

    def _install_shortcuts(self) -> None:
        for seq, slot in (
            (QKeySequence("Ctrl+F"), self.search.setFocus),
            (QKeySequence("Ctrl+Return"), self._deploy),
            (QKeySequence("Ctrl+R"), self._reload_catalog),
        ):
            act = QAction(self)
            act.setShortcut(seq)
            act.triggered.connect(slot)
            self.addAction(act)

    def _reload_catalog(self) -> None:
        self.log.info(f"Loading TLE catalog from {self.cache_dir} …")
        cat = load_catalog(self.cache_dir)
        if not cat.by_id:
            self.log.warn("Catalog is empty — run `./VisibleEphemeris --refresh` or point at a tle_cache directory.")
        else:
            self.log.ok(f"Indexed {len(cat.by_id)} unique satellites from {self.cache_dir}.")
        self.catalog = cat

        # Preserve selected IDs across reloads.
        self._model = CatalogModel(self.catalog, set(self.selected.keys()))
        self._model.itemChanged.connect(self._on_catalog_item_changed)
        self.proxy = SearchProxy(self)
        self.proxy.setSourceModel(self._model)
        self.catalog_view.setModel(self.proxy)
        hh = self.catalog_view.horizontalHeader()
        hh.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.catalog_view.setColumnWidth(1, 90)
        try:
            self.search.textChanged.disconnect()
        except TypeError:
            pass
        self.search.textChanged.connect(self._on_search)
        self._refresh_counts()
        self._refresh_fleet_view()

    def _pick_cache_dir(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "Select tle_cache directory", self.cache_dir)
        if d:
            self.cache_dir = d
            self._reload_catalog()
            self.status.showMessage(f"Cache: {self.cache_dir}   Config: {self.config_path}")

    def _on_search(self, text: str) -> None:
        try:
            self.proxy.setFilterRegularExpression(text)
        except Exception:
            self.proxy.setFilterFixedString(text)
        self._refresh_counts()

    def _refresh_counts(self) -> None:
        total = self._model.rowCount() if hasattr(self, "_model") else 0
        shown = self.proxy.rowCount() if hasattr(self, "proxy") else 0
        self.catalog_count.setText(f"{shown} / {total} shown")
        self.fleet_count.setText(f"{len(self.selected)} selected")

    def _on_catalog_item_changed(self, item: QStandardItem) -> None:
        if item.column() != 0 or not item.isCheckable():
            return
        sat: Optional[Satellite] = item.data(Qt.ItemDataRole.UserRole)
        if sat is None:
            return
        if item.checkState() == Qt.CheckState.Checked:
            self.selected[sat.id] = sat
        else:
            self.selected.pop(sat.id, None)
        self._refresh_fleet_view()

    def _add_highlighted(self) -> None:
        sel = self.catalog_view.selectionModel()
        if sel is None:
            return
        for idx in sel.selectedRows(0):
            src_idx = self.proxy.mapToSource(idx)
            item = self._model.itemFromIndex(src_idx)
            if item is not None and item.checkState() != Qt.CheckState.Checked:
                item.setCheckState(Qt.CheckState.Checked)

    def _add_all_filtered(self) -> None:
        n = self.proxy.rowCount()
        if n > 5000:
            r = QMessageBox.question(
                self, "Add all filtered",
                f"Add {n} satellites to the fleet? Very large fleets slow the tracker down.",
            )
            if r != QMessageBox.StandardButton.Yes:
                return
        for row in range(n):
            src_idx = self.proxy.mapToSource(self.proxy.index(row, 0))
            item = self._model.itemFromIndex(src_idx)
            if item is not None and item.checkState() != Qt.CheckState.Checked:
                item.setCheckState(Qt.CheckState.Checked)

    def _refresh_fleet_view(self) -> None:
        self.fleet_list.clear()
        for sat in sorted(self.selected.values(), key=lambda s: s.name.lower()):
            self.fleet_list.addItem(QListWidgetItem(f"{sat.name:<24} [{sat.id}]  {sat.source}"))
        self._refresh_counts()

    def _remove_from_fleet(self) -> None:
        rows = [i.text() for i in self.fleet_list.selectedItems()]
        for text in rows:
            # last bracketed number is the NORAD ID
            m = re.search(r"\[(\d+)\]", text)
            if m:
                nid = int(m.group(1))
                self.selected.pop(nid, None)
                # also uncheck in catalog model
                for row in range(self._model.rowCount()):
                    item = self._model.item(row, 0)
                    sat: Satellite = item.data(Qt.ItemDataRole.UserRole)
                    if sat and sat.id == nid:
                        item.setCheckState(Qt.CheckState.Unchecked)
                        break
        self._refresh_fleet_view()

    def _clear_fleet(self) -> None:
        if not self.selected:
            return
        r = QMessageBox.question(self, "Clear fleet", f"Remove all {len(self.selected)} satellites?")
        if r != QMessageBox.StandardButton.Yes:
            return
        self.selected.clear()
        for row in range(self._model.rowCount()):
            item = self._model.item(row, 0)
            if item.checkState() == Qt.CheckState.Checked:
                item.setCheckState(Qt.CheckState.Unchecked)
        self._refresh_fleet_view()

    def _deploy(self) -> None:
        if not self.selected:
            QMessageBox.warning(self, "Cannot deploy", "The fleet is empty. Select at least one satellite.")
            return
        group = sanitise_group(self.group_name.text())
        tle_path = os.path.join(self.cache_dir, f"{group}.txt")
        try:
            os.makedirs(self.cache_dir, exist_ok=True)
            with open(tle_path, "w", encoding="utf-8") as f:
                for sat in self.selected.values():
                    f.write(f"{sat.name}\n{sat.l1}\n{sat.l2}\n")
        except OSError as e:
            self.log.error(f"Failed to write {tle_path}: {e}")
            QMessageBox.critical(self, "Write failed", str(e))
            return
        self.log.ok(f"Wrote {len(self.selected)} TLEs to {tle_path}")

        self.cfg["lat"] = self.lat.value()
        self.cfg["lon"] = self.lon.value()
        self.cfg["alt"] = self.alt.value()
        self.cfg["min_el"] = self.min_el.value()
        self.cfg["max_apo"] = self.max_apo.value()
        self.cfg["show_all_visible"] = self.radio_mode.isChecked()
        self.cfg["group_selection"] = group
        self.cfg["sat_selection"] = ""
        try:
            save_config(self.config_path, self.cfg)
        except OSError as e:
            self.log.error(f"Failed to write {self.config_path}: {e}")
            QMessageBox.critical(self, "Write failed", str(e))
            return
        self.log.ok(f"Updated {self.config_path} — group_selection='{group}'")
        self.status.showMessage(f"Deployed group '{group}' — {len(self.selected)} sats", 5000)
        QMessageBox.information(
            self, "Mission deployed",
            f"Group '{group}' written with {len(self.selected)} satellites.\n"
            f"Run the tracker (`./VisibleEphemeris` or `python -m python_tracker.main`) to launch.",
        )


def main(argv: Optional[list[str]] = None) -> int:
    app = QApplication(argv if argv is not None else sys.argv)
    apply_dark_theme(app)
    win = ArchitectWindow()
    win.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
