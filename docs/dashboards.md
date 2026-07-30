# Visible Ephemeris — Dashboards Guide

Visible Ephemeris ships **six** ways to look at what the tracker is doing.
Three run in the browser or a terminal (they've been part of the project since
the beginning) and three are new **PyQt6 / Qt 6 dashboards** built for
day-to-day operational and development use on Linux.

| Dashboard                    | Kind         | Default endpoint / launcher            | Best for |
| ---------------------------- | ------------ | -------------------------------------- | -------- |
| Web mission dashboard        | Browser      | `http://localhost:8080`                | Remote viewing, phones, tablets |
| HTTP text mirror             | Browser      | `http://localhost:12345`               | Auto-refreshing plain-text feed |
| Physics stream (TCP)         | TCP client   | `localhost:12346`                      | Custom scripts, `tracking_client.py` |
| Terminal Orbital Architect   | ANSI CLI     | `python3 orbital_architect.py`         | Servers with no display |
| **Orbital Architect (Qt)**   | Qt 6 GUI     | `ve-orbital-architect-qt`              | Ergonomic satellite selection |
| **HPOP diagnostic panel**    | Qt 6 GUI     | `ve-hpop-panel`                        | Live orbit-element strip charts |
| **`ve-ide` (VS Code-style)** | Qt 6 IDE     | `ve-ide [project_root]`                | Development inside a familiar shell |

The three Qt dashboards use the **latest Qt available to the interpreter**
(tested on both the Ubuntu system PyQt6 6.6 / Qt 6.4 and the pip-installed
PyQt6 6.11 / Qt 6.11 wheels — the code is Qt-6.4-through-6.11 compatible) and
share the palette, fonts, and keyboard model of VS Code so users get
zero-friction familiarity.

Confirm your Qt version with:

    python3 -c "from PyQt6.QtCore import QT_VERSION_STR; print(QT_VERSION_STR)"

---

## Prerequisites

The dashboards depend on:

    python3      >= 3.10
    PyQt6        >= 6.6   (Qt 6.4+, tested through Qt 6.11)
    pyqtgraph    >= 0.13
    matplotlib   >= 3.6
    numpy PyYAML requests skyfield

There are two supported ways to get these:

**A. pip venv (self-contained, everything from `requirements.txt`)**

    python3 -m venv venv
    ./venv/bin/pip install -r python_tracker/requirements.txt

That pulls PyQt6 with its own bundled Qt runtime, so the venv works no matter
what Qt is (or isn't) on the host system. This is the developer default.

**B. system packages (Debian / Ubuntu / Mint)**

The `install.sh` and `.deb` package take this route so end-users don't need to
compile anything:

    sudo apt install python3-pyqt6 python3-pyqtgraph python3-matplotlib \
                     python3-numpy python3-yaml python3-requests python3-skyfield

Either path works; `ve-ide` auto-detects a project venv (`venv/`, `.venv/`,
`env/`) and prefers it for its Run buttons, falling back to the interpreter
that started the IDE.

Run the dashboards from a source checkout with:

    ./venv/bin/python3 -m python_tracker.qt_dashboards.ve_ide       # IDE
    ./venv/bin/python3 -m python_tracker.qt_dashboards.orbital_architect_qt
    ./venv/bin/python3 -m python_tracker.qt_dashboards.hpop_panel

or, after `./install.sh` (or the `.deb`), via the short launchers `ve-ide`,
`ve-orbital-architect-qt`, `ve-hpop-panel`.

### Launching the tracker from ve-ide

The ve-ide **▶ Run tracker** and **▶ Run tracker with --hpop** buttons work
even though the IDE is a non-terminal parent: `python_tracker/main.py` detects
that stdin isn't a TTY and skips its interactive keyboard poller and the
on-exit "Save config?" prompt. Output streams into the **Output** tab of the
bottom dock, and the **Satellites** tab (also in the bottom dock) can connect
to the tracker's `:12346` physics feed to show the live fleet inside the IDE.
Stop the tracker with **Ctrl+.** or the command palette's *Run: Stop process*.

---

## 1 · Orbital Architect (Qt)  —  `ve-orbital-architect-qt`

**Purpose.** Curate a fleet of satellites to track, then deploy the choice into
`tle_cache/<group>.txt` and `config.yaml`. Same job as the ANSI
`orbital_architect.py` tool, but with a modern picker UI.

**Ergonomic layout.**

    ┌────────────────────────────────────────────────────────────────────┐
    │  Orbital Architect                    [Reload]  [Choose tle_cache…]│
    ├──────────────────────────────┬─────────────────────────────────────┤
    │  Search  [                  ]│  Selected fleet                     │
    │  ┌────────────────────────┐  │  ┌───────────────────────────────┐  │
    │  │☐ ISS (ZARYA)   25544   │  │  │ NOAA 15         [25338]        │  │
    │  │☑ NOAA 15       25338   │  │  │ ISS (ZARYA)     [25544]        │  │
    │  │☐ STARLINK-3021 55220…  │  │  │ …                              │  │
    │  │…                       │  │  └───────────────────────────────┘  │
    │  └────────────────────────┘  │  [← Remove]  [Clear]   3 selected   │
    │  [Add highlighted →][Add all]│                                     │
    │                              │  ┌ Station & mode ─────────────┐    │
    │                              │  │ Lat  [ 39.5478 ]            │    │
    │                              │  │ Lon  [ -76.0916]            │    │
    │                              │  │ Alt  [  0.100 ] km          │    │
    │                              │  │ Min el [ 0.0 ] °            │    │
    │                              │  │ ☑ Radio mode (show all)     │    │
    │                              │  └────────────────────────────┘    │
    │                              │  Group [user_defined]  [Deploy ▶]  │
    └──────────────────────────────┴─────────────────────────────────────┘
    │ [log lines]                                                        │
    ├────────────────────────────────────────────────────────────────────┤
    │ status bar: cache dir · config path                     ● online   │
    └────────────────────────────────────────────────────────────────────┘

**Keyboard shortcuts.**

* `Ctrl+F` — jump to search box
* `Ctrl+R` — reload catalog
* `Ctrl+Return` — deploy

**Where it reads and writes.**

* Reads every `tle_cache/*.txt` (skips `user_defined.txt` to avoid
  reading back its own output).
* Writes the current fleet as `tle_cache/<group>.txt` (three-line TLE format).
* Updates `config.yaml` with `lat`, `lon`, `alt`, `min_el`, `max_apo`,
  `show_all_visible`, `group_selection`, and clears `sat_selection`.

Both files land in the current working directory. If neither `tle_cache/` nor
`config.yaml` exists in the CWD the tool walks up one level (repo root) before
falling back to the defaults.

---

## 2 · HPOP diagnostic panel  —  `ve-hpop-panel`

**Purpose.** Connect to the running tracker's TCP physics stream (default
`127.0.0.1:12346`) and render real-time diagnostics of the fleet plus deep
introspection into one selected satellite. This is where you go when the C++
or Python tracker has been launched with `--hpop` and you want to see the
HPOP integrator's output as it happens.

**Layout.**

    ┌──── Header: Host [127.0.0.1] Port [12346] [Reconnect] ────────────┐
    │ ┌ Current frame ────────────────────────────────────────────────┐ │
    │ │ Timestamp       2026-07-25 03:14:07                           │ │
    │ │ Observer        OBS: 39.5, -76.1 | SHOWN: 87                  │ │
    │ │ Visible/shown   42 / 87       Selected  ISS (ZARYA) [25544]  │ │
    │ └───────────────────────────────────────────────────────────────┘ │
    │ ┌──────────────────────────┬──────────────────────────────────┐   │
    │ │ Name   NORAD  El° Range… │  Elevation (deg)                 │   │
    │ │ ISS…   25544  45.6 500…  │  ▄▆█▇▆▄▂▂▄▆█▇▆                   │   │
    │ │ …                        ├──────────────────────────────────┤   │
    │ │                          │  Range (km)                       │   │
    │ │                          ├──────────────────────────────────┤   │
    │ │                          │  Range rate (km/s)                │   │
    │ │                          ├──────────────────────────────────┤   │
    │ │                          │  Visible-count over time          │   │
    │ └──────────────────────────┴──────────────────────────────────┘   │
    │ ● connected 127.0.0.1:12346                                        │
    └────────────────────────────────────────────────────────────────────┘

**Behaviour.**

* Automatically **reconnects** every few seconds if the tracker goes away.
* Buffers **600 samples per satellite** for the four strip charts (`HISTORY`
  constant in `hpop_panel.py`).
* Clicking any row in the table selects that satellite; the three per-sat
  strip charts follow the selection.
* `F4` toggles the **Raw stream** dock — useful for diagnosing frame-parse
  problems.
* Table columns are sortable (click header). The frame parser survives
  malformed rows.

**Custom host / port.**

    ve-hpop-panel --host 10.0.0.5 --port 12346

---

## 3 · `ve-ide`  —  VS Code-style IDE

**Purpose.** A full VS Code-like Qt shell so you can develop, run, and monitor
the tracker in a single window. It is deliberately familiar to VS Code users:
the activity bar, tabbed editor, bottom panel with terminal and output tabs,
`Ctrl+P` quick-open, and `Ctrl+Shift+P` command palette all behave the way you
expect.

**Layout.**

    ┌──┬────────────┬────────────────────────────────────────────┐
    │E │ EXPLORER   │  main.cpp × │ config.yaml ×                 │
    │S │ ▼ include/ │                                            │
    │R │   ephemeris│  1  #include <ephemeris.hpp>               │
    │S │   ...      │  2                                         │
    │  │ ▼ python/  │  3  int main() { … }                       │
    │A │   main.py  │  …                                         │
    │G │   ...      │                                            │
    │  ├────────────┴────────────────────────────────────────────┤
    │  │ Terminal │ Output │ Problems │ Satellites               │
    │  │ $ ctest --test-dir build --output-on-failure           │
    │  │ …                                                       │
    ├──┴─────────────────────────────────────────────────────────┤
    │ Root: /home/user/VisibleEphemerisCPP    ● offline          │
    └────────────────────────────────────────────────────────────┘

The activity-bar letters `E S R S G` map to Explorer, Search, Run, Satellites,
Settings. Each activity swaps the side panel to its own widget.

**Side-panel views.**

* **Explorer** — `QFileSystemModel` tree; double-click any file to open a tab.
* **Search** — regex search across the project, respecting `.git/`,
  `build/`, `venv/`, `__pycache__/`, `node_modules/` exclusions. An include
  glob (`*.py`, `*.cpp`) narrows the search. Activate a hit to jump to the
  line.
* **Run** — one-click launchers:
    * `▶ Run tracker (python)`  →  `python3 -m python_tracker.main`
    * `▶ Run tracker with --hpop`
    * `▶ Orbital Architect (Qt)`  /  `▶ HPOP diagnostic panel`
    * `▶ CMake configure` / `▶ CMake build` / `▶ ctest`
    * `▶ Run C++ VisibleEphemeris`  (`./build/VisibleEphemeris`)
    * Plus a custom command box (`Enter` to run).

  Output streams into the **Output** tab of the bottom dock in real time.
* **Satellites** — placeholder that focuses the bottom Satellites tab (see
  below).
* **Settings** — change project root, editor font size.

**Bottom-panel tabs.**

* **Terminal** — a minimal `QProcess`-backed shell. Type `cd`, `ls`, `pytest…`
  etc. `cd`  and `clear` are handled in-process; everything else goes through
  `/bin/sh -c` so pipes and redirects work.
* **Output** — colour-coded log stream (info/ok/warn/error) fed by the Run
  buttons and command palette.
* **Problems** — reserved for future integration with build/warning output.
* **Satellites** — enters host/port, connects to the tracker's physics stream
  (`:12346` by default), and shows the live table. This means you can run the
  tracker inside `ve-ide` and watch the fleet without leaving the window.

**Editor.**

* Tabbed multi-buffer editor. Dirty tabs show a leading `●`.
* Ctrl+S saves; Ctrl+Shift+S saves all. Close-tab prompts if dirty.
* Minimal syntax highlighting for Python, C/C++, and YAML — deliberately
  small (no external dependencies) but recognisable.

**Command palette (`Ctrl+Shift+P`).**

    File: New           Ctrl+N
    File: Open…         Ctrl+O
    File: Save          Ctrl+S
    View: Toggle side panel   Ctrl+B
    View: Toggle bottom panel Ctrl+J
    View: Focus terminal      Ctrl+`
    Run: Tracker              F5
    Run: CMake build          Ctrl+Shift+B
    Run: ctest
    Run: Stop process         Ctrl+.
    Dashboards: Orbital Architect
    Dashboards: HPOP panel
    …

**Quick open (`Ctrl+P`).** Fuzzy-filter list of every non-`.git`/`build`/`venv`
file in the project.

---

## 4 · Web mission dashboard  —  `http://localhost:8080`

Served by `python_tracker/web_server.py` (FastAPI + uvicorn). Loaded from
`python_tracker/static/index.html` with a JSON feed at `/api/satellites`.
Ideal for phones, tablets, and remote viewing.

## 5 · HTTP text mirror  —  `http://localhost:12345`

Simple auto-refresh HTML page produced by `python_tracker/text_server.py`.
Useful when you want to point a browser at the tracker but don't need the JS
map. The C++ tracker exposes an equivalent server at the same port.

## 6 · Physics TCP stream  —  `localhost:12346`

Text frames separated by `---END_FRAME---`. The Qt HPOP panel above consumes
it, as does the CLI utility `tools/tracking_client.py`. Full frame format:

    VISIBLE EPHEMERIS v12.65-CODE-ONLY
    2026-07-25 03:14:07 LOC
    OBS: 39.5, -76.1 | SHOWN: 87

    NAME             AZ       EL      RANGE  RR(km/s)   VIS  NEXT EVENT    NORAD        LAT        LON     APOGEE  FLARE
    ------------------------------------------------------------------------------------------------------------------
    ISS (ZARYA)      123.4    45.6    500.2    -1.234  VIS  Sunset        25544    45.6789   123.4567     412.5      0
    …
    ---END_FRAME---

The `HPOPWindow.parse_frame()` function in
[`python_tracker/qt_dashboards/hpop_panel.py`](../python_tracker/qt_dashboards/hpop_panel.py)
is the reference parser.

---

## Extending the dashboards

The Qt dashboards live in `python_tracker/qt_dashboards/`:

    __init__.py                    package + version
    theme.py                       VS Code Dark+ palette, fonts, widgets
                                   (LogPane, StatusBar, Card, SectionLabel)
    orbital_architect_qt.py        Fleet selector
    hpop_panel.py                  Physics-stream diagnostic panel
                                   (also exports PhysicsClient + Frame,
                                   reused by ve_ide's Satellites tab)
    ve_ide.py                      IDE shell

To add a new dashboard:

1. Create `python_tracker/qt_dashboards/my_panel.py` with a `main()` entry
   point.
2. Import `apply_dark_theme`, `LogPane`, `StatusBar`, `PALETTE`,
   `monospace_font`, and `ui_font` from `.theme` so the visual language stays
   consistent.
3. Add a launcher under `bin/` following the pattern of `bin/ve-hpop-panel`.
4. Add a `.desktop` file under `packaging/desktop/`.
5. Wire the new launcher into `install.sh` (`write_launcher` line) and
   `packaging/debian/rules` (`override_dh_auto_install` loop).

---

## Troubleshooting

**"No module named PyQt6"** — either `pip install -r python_tracker/requirements.txt`
into your venv (self-contained PyQt6 wheel with bundled Qt) or
`sudo apt install python3-pyqt6` (system Qt). Both work; `install.sh --user`
handles it for you.

**"Address already in use" on :8080 / :12345 / :12346** — another tracker is
already bound to those ports (commonly the C++ `VisibleEphemeris` binary).
Check with `ss -tlnp | grep -E ':(8080|12345|12346)\s'` and stop the other
instance, or rebind the C++ tracker with `./VisibleEphemeris --port 8090,12355,12356`.
The Python tracker prints the bind errors but continues without those servers.

**Dashboard opens but shows "offline"** — the tracker isn't running or the
port doesn't match. Start the tracker (`ve-tracker` or `./VisibleEphemeris`),
confirm `PhysicsServer started on port 12346` in its output, then hit
**Reconnect** in the panel.

**"QThread: Destroyed while thread is still running" — Qt 6.11+** — the
`PhysicsClient` background thread hooks `QApplication.aboutToQuit` and stops
itself before the app tears down. If you subclass or add another `QThread`,
follow the same pattern.

**Qt segfault under `QT_QPA_PLATFORM=offscreen`** — some `QHeaderView` calls
segfault on the offscreen platform when the header has no sections yet. The
codebase guards against this by only touching header sections after a model
is set; if you're extending the code, follow the same pattern.

**`propagateSizeHints()` warnings** — harmless offscreen-only noise. Ignore.

**`ve-ide` Run tracker fails with "No module named skyfield"** — the IDE fell
back to the interpreter that started it (probably `/usr/bin/python3`). Either
create a `venv/` at the project root (`python3 -m venv venv &&
./venv/bin/pip install -r python_tracker/requirements.txt`) so `_project_python()`
auto-detects it, or install skyfield/fastapi/uvicorn into the system Python.
The Run panel's header line shows which interpreter will be used.

**Font looks wrong** — install `fonts-firacode` for the best experience;
the theme falls back through JetBrains Mono → Cascadia Code → DejaVu Sans
Mono → generic monospace, so the dashboards still work without it.
