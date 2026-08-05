#!/usr/bin/env bash
# install.sh — one-line installer for Visible Ephemeris (Python tracker + Qt
# dashboards + CLI + desktop entries). Works on Debian / Ubuntu / Mint.
#
# Usage:
#     ./install.sh              # user install (~/.local, no sudo)
#     ./install.sh --system     # system-wide install (needs sudo)
#     ./install.sh --uninstall  # remove installed launchers & desktop entries
#     ./install.sh --help
#
# One-liner (from a checkout):
#     bash install.sh
#
# What it does
# ------------
#   1. Confirms Python 3.10+ is present.
#   2. Installs OS packages we depend on (PyQt6, pyqtgraph, matplotlib, …).
#      Skipped with --no-apt.
#   3. Copies the Python tracker + qt_dashboards package into a "runtime root":
#         --user       ->  $HOME/.local/lib/visible-ephemeris/
#         --system     ->  /usr/lib/visible-ephemeris/
#   4. Installs launchers `ve-ide`, `ve-orbital-architect-qt`, `ve-hpop-panel`,
#      `ve-tracker` into $HOME/.local/bin or /usr/local/bin. Each launcher
#      exports `VE_PY_ROOT` so it finds its Python package.
#   5. Installs desktop entries into $HOME/.local/share/applications or
#      /usr/share/applications so the dashboards show up in your app menu.
#
# The install is fully idempotent — re-running is safe and just refreshes
# everything.

set -euo pipefail

# ---------- arg parsing ----------
MODE="user"
DO_APT=1
UNINSTALL=0
VERBOSE=0

usage() {
    sed -n '2,32p' "$0"
    exit "${1:-0}"
}

for a in "$@"; do
    case "$a" in
        -h|--help) usage 0 ;;
        --system)  MODE="system" ;;
        --user)    MODE="user" ;;
        --no-apt)  DO_APT=0 ;;
        --uninstall) UNINSTALL=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        *) echo "unknown option: $a" >&2; usage 2 ;;
    esac
done

log()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[OK]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[WARN]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------- locate repo & runtime targets ----------
REPO_ROOT="$(cd "$(dirname "$(readlink -f "$0" 2>/dev/null || echo "$0")")" && pwd)"
cd "$REPO_ROOT"

if [ "$MODE" = "system" ]; then
    PY_ROOT="/usr/lib/visible-ephemeris"
    BIN_DIR="/usr/local/bin"
    APPS_DIR="/usr/share/applications"
    SUDO="sudo"
else
    PY_ROOT="$HOME/.local/lib/visible-ephemeris"
    BIN_DIR="$HOME/.local/bin"
    APPS_DIR="$HOME/.local/share/applications"
    SUDO=""
fi

# ---------- uninstall ----------
if [ "$UNINSTALL" = "1" ]; then
    log "Removing Visible Ephemeris ($MODE install)…"
    $SUDO rm -rf "$PY_ROOT"
    for f in ve-ide ve-orbital-architect-qt ve-hpop-panel ve-live-od ve-tracker; do
        $SUDO rm -f "$BIN_DIR/$f"
    done
    for f in ve-ide.desktop ve-orbital-architect-qt.desktop ve-hpop-panel.desktop ve-live-od.desktop; do
        $SUDO rm -f "$APPS_DIR/$f"
    done
    ok "Uninstalled."
    exit 0
fi

# ---------- python check ----------
log "Checking Python…"
if ! command -v python3 >/dev/null 2>&1; then
    fail "python3 not found — please install Python 3.10 or newer."
fi
PY_VER="$(python3 -c 'import sys;print("%d.%d"%sys.version_info[:2])')"
python3 -c 'import sys; sys.exit(0 if sys.version_info>=(3,10) else 1)' \
    || fail "Python 3.10+ required (found $PY_VER)."
ok "Python $PY_VER"

# ---------- apt deps ----------
APT_PKGS=(
    python3-pyqt6
    python3-pyqt6.qtsvg
    python3-pyqtgraph
    python3-matplotlib
    python3-numpy
    python3-yaml
    python3-requests
)
# Optional (pulled if available; tracker also runs without them via pip venv fallbacks).
APT_OPT_PKGS=(
    python3-skyfield
    python3-fastapi
    python3-uvicorn
)

if [ "$DO_APT" = "1" ]; then
    if command -v apt-get >/dev/null 2>&1; then
        log "Installing apt dependencies…"
        SUDO_APT="sudo"
        [ "$(id -u)" = "0" ] && SUDO_APT=""
        $SUDO_APT apt-get update -qq
        $SUDO_APT apt-get install -y --no-install-recommends "${APT_PKGS[@]}"
        # Best-effort optional packages.
        for p in "${APT_OPT_PKGS[@]}"; do
            $SUDO_APT apt-get install -y --no-install-recommends "$p" 2>/dev/null \
                || warn "optional package $p not installed (that's OK; requirements.txt has fallbacks)."
        done
        ok "apt dependencies installed."
    else
        warn "apt-get not found — skipping OS package install. On non-Debian systems, install PyQt6, pyqtgraph, matplotlib, numpy, PyYAML manually."
    fi
else
    log "Skipping apt install (--no-apt)."
fi

# ---------- copy python tree ----------
log "Installing Python tree to $PY_ROOT"
$SUDO mkdir -p "$PY_ROOT"
# Use rsync if available for nicer output; fall back to cp.
if command -v rsync >/dev/null 2>&1; then
    $SUDO rsync -a --delete \
        --exclude '__pycache__' --exclude '*.pyc' --exclude '.pytest_cache' \
        "$REPO_ROOT/python_tracker/" "$PY_ROOT/python_tracker/"
else
    $SUDO rm -rf "$PY_ROOT/python_tracker"
    $SUDO cp -a "$REPO_ROOT/python_tracker" "$PY_ROOT/"
fi
ok "Python tree copied."

# ---------- launchers ----------
# Mapping: launcher name -> python module.
write_launcher() {
    local name="$1"; local module="$2"; local dst="$BIN_DIR/$name"
    $SUDO tee "$dst" >/dev/null <<EOF
#!/bin/sh
# Installed launcher for Visible Ephemeris — generated by install.sh
export VE_PY_ROOT="$PY_ROOT"
PY="\${VE_PYTHON:-python3}"
export PYTHONPATH="\$VE_PY_ROOT\${PYTHONPATH:+:\$PYTHONPATH}"
exec "\$PY" -m $module "\$@"
EOF
    $SUDO chmod +x "$dst"
}

log "Installing launchers to $BIN_DIR"
$SUDO mkdir -p "$BIN_DIR"
write_launcher ve-ide                  python_tracker.qt_dashboards.ve_ide
write_launcher ve-orbital-architect-qt python_tracker.qt_dashboards.orbital_architect_qt
write_launcher ve-hpop-panel           python_tracker.qt_dashboards.hpop_panel
write_launcher ve-live-od              python_tracker.qt_dashboards.live_od_panel

write_launcher ve-tracker              python_tracker.main
ok "Launchers installed: ve-ide  ve-orbital-architect-qt  ve-hpop-panel  ve-live-od  ve-tracker"

# ---------- desktop entries ----------
log "Installing desktop entries to $APPS_DIR"
$SUDO mkdir -p "$APPS_DIR"
for f in ve-ide.desktop ve-orbital-architect-qt.desktop ve-hpop-panel.desktop ve-live-od.desktop; do
    if [ -f "$REPO_ROOT/packaging/desktop/$f" ]; then
        $SUDO install -m 0644 "$REPO_ROOT/packaging/desktop/$f" "$APPS_DIR/$f"
    fi
done
if command -v update-desktop-database >/dev/null 2>&1; then
    $SUDO update-desktop-database -q "$APPS_DIR" 2>/dev/null || true
fi
ok "Desktop entries installed."

# ---------- final smoke test ----------
log "Smoke-testing dashboard imports…"
if VE_PY_ROOT="$PY_ROOT" PYTHONPATH="$PY_ROOT" QT_QPA_PLATFORM=offscreen \
    python3 -c 'import python_tracker.qt_dashboards.ve_ide, python_tracker.qt_dashboards.hpop_panel, python_tracker.qt_dashboards.orbital_architect_qt; print("ok")' \
    >/dev/null 2>&1; then
    ok "All Qt dashboards import cleanly."
else
    warn "Import smoke test failed — check PyQt6 installation."
fi

cat <<EOF

  ╭──────────────────────────────────────────────────────────╮
  │  Visible Ephemeris installed ($MODE mode).               │
  │                                                          │
  │    ve-ide                    — VS Code-style IDE         │
  │    ve-orbital-architect-qt   — Fleet selector            │
  │    ve-hpop-panel             — Physics stream monitor    │
  │    ve-tracker                — CLI + web + text tracker  │
  ╰──────────────────────────────────────────────────────────╯

EOF

if [ "$MODE" = "user" ]; then
    case ":$PATH:" in
        *":$BIN_DIR:"*) : ;;
        *) warn "$BIN_DIR is not on your PATH. Add:  export PATH=\"\$HOME/.local/bin:\$PATH\"  to your shell rc." ;;
    esac
fi
