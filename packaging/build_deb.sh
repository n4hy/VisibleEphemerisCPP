#!/usr/bin/env bash
# packaging/build_deb.sh — build a visible-ephemeris_<ver>_all.deb.
#
# Runs from a repo checkout. Copies the tree into ../build-deb/visible-ephemeris,
# stages the debian/ metadata (which lives under packaging/debian in-tree so it
# doesn't clutter the source root), and then invokes dpkg-buildpackage. The
# resulting .deb lands in ../build-deb/.
#
# Prereqs (installed by install.sh --system, or manually):
#     sudo apt-get install -y debhelper dpkg-dev fakeroot
#
# Usage:
#     packaging/build_deb.sh
#     packaging/build_deb.sh --clean          # remove build-deb/ first
#     packaging/build_deb.sh --install        # apt install the built .deb

set -euo pipefail

CLEAN=0
DO_INSTALL=0
for a in "$@"; do
    case "$a" in
        --clean)   CLEAN=1 ;;
        --install) DO_INSTALL=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

REPO="$(cd "$(dirname "$(readlink -f "$0" 2>/dev/null || echo "$0")")/.." && pwd)"
STAGE_PARENT="$REPO/../build-deb"
STAGE="$STAGE_PARENT/visible-ephemeris"

[ "$CLEAN" = "1" ] && rm -rf "$STAGE_PARENT"
mkdir -p "$STAGE_PARENT"

echo "==> Staging repo into $STAGE"
rsync -a --delete \
    --exclude '.git' --exclude 'build' --exclude 'venv' \
    --exclude '__pycache__' --exclude '*.pyc' \
    --exclude '*.deb' --exclude 'build-deb' \
    "$REPO/" "$STAGE/"

# Stage the debian/ metadata at the source root (dpkg-buildpackage wants it there).
rm -rf "$STAGE/debian"
cp -a "$REPO/packaging/debian" "$STAGE/debian"
chmod +x "$STAGE/debian/rules"

echo "==> Running dpkg-buildpackage in $STAGE"
cd "$STAGE"
dpkg-buildpackage -us -uc -b -rfakeroot

DEB="$(ls -1t "$STAGE_PARENT"/visible-ephemeris_*_all.deb 2>/dev/null | head -1 || true)"
if [ -n "$DEB" ] && [ -f "$DEB" ]; then
    echo
    echo "==> Built: $DEB"
    dpkg-deb -I "$DEB" | head -20
    if [ "$DO_INSTALL" = "1" ]; then
        echo "==> Installing $DEB"
        sudo apt-get install -y "$DEB"
    fi
else
    echo "!! no .deb produced — check dpkg-buildpackage output above." >&2
    exit 1
fi
