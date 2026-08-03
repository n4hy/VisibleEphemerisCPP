# Visible Ephemeris — Installation Guide

Three supported install paths on Linux:

| Path                    | Best for                                | Command |
| ----------------------- | --------------------------------------- | ------- |
| **`install.sh`**        | Personal machines, quick evaluation     | `./install.sh` |
| **`.deb` package**      | Fleets of Debian/Ubuntu boxes, sysadmin | `packaging/build_deb.sh --install` |
| **Source / venv**       | Contributors, developers                | `python3 -m venv .venv && ./.venv/bin/pip install -r python_tracker/requirements.txt` |

The C++ tracker binary (`./VisibleEphemeris`) still uses the existing CMake
build; see the top-level [`README.md`](../README.md#build). The paths below
cover the **Python tracker and Qt dashboards** only.

The **orbit-determination (OD) subsystem** (see [`orbit_determination.md`](orbit_determination.md))
is **opt-in** at CMake configure time (`-DBUILD_OD=ON`) and adds two
sibling-library requirements — see the dedicated section
[Optional: OD subsystem dependencies](#optional-od-subsystem-dependencies)
below. Nothing else in the tracker requires them.

---

## 1 · `install.sh` — the one-line installer

From a repo checkout:

    ./install.sh              # user install into ~/.local (no sudo)
    ./install.sh --system     # system install into /usr/local (needs sudo)

After it finishes you'll have four launchers on your PATH:

    ve-ide                    VS Code-style Qt IDE
    ve-orbital-architect-qt   Fleet selector
    ve-hpop-panel             Physics stream monitor
    ve-tracker                CLI + web + text tracker

and three application-menu entries under **Science / Development**.

**Flags.**

    --user         install under $HOME/.local (default)
    --system       install under /usr/local  (uses sudo)
    --no-apt       don't run apt-get (use if you manage packages yourself)
    --uninstall    remove everything install.sh put in place
    --help         print the header comment

**What it does, in one line each.**

1. Verifies Python 3.10+.
2. `apt-get install -y python3-pyqt6 python3-pyqtgraph python3-matplotlib …`
   (unless `--no-apt`).
3. Copies `python_tracker/` into `$HOME/.local/lib/visible-ephemeris/`
   (user) or `/usr/lib/visible-ephemeris/` (system).
4. Writes launcher shell scripts under `$HOME/.local/bin` or `/usr/local/bin`
   that export `VE_PY_ROOT=<install prefix>` and exec the Python module.
5. Installs `.desktop` files under `$HOME/.local/share/applications` or
   `/usr/share/applications` and runs `update-desktop-database`.
6. Smoke-tests the imports (`QT_QPA_PLATFORM=offscreen python3 -c "import …"`)
   and prints a summary box.

The install is **idempotent** — re-running just refreshes the tree. To roll
back, run `./install.sh --uninstall` in the same mode you installed with.

If `$HOME/.local/bin` isn't on your PATH, `install.sh` prints a reminder:

    export PATH="$HOME/.local/bin:$PATH"

Add that to `~/.bashrc` (or `~/.zshrc`).

---

## 2 · `.deb` package

Build a native Debian package from the same source tree:

    # one-time build tools
    sudo apt-get install -y debhelper dpkg-dev fakeroot rsync

    # build the .deb
    packaging/build_deb.sh --clean

    # install it (adds --install to apt-install the just-built .deb)
    packaging/build_deb.sh --clean --install

The build helper stages a clean copy of the repo into `../build-deb/` and
runs `dpkg-buildpackage -us -uc -b -rfakeroot`. Output:

    ../build-deb/visible-ephemeris_1.0.0-1_all.deb

Install manually with:

    sudo apt-get install ../build-deb/visible-ephemeris_1.0.0-1_all.deb

The package puts:

    /usr/lib/visible-ephemeris/python_tracker/…    (Python code)
    /usr/bin/ve-ide  ve-orbital-architect-qt
    /usr/bin/ve-hpop-panel  ve-tracker             (launchers)
    /usr/share/applications/ve-*.desktop           (menu entries)
    /usr/share/doc/visible-ephemeris/              (README + docs)

Dependencies (declared in `debian/control`):

    Depends:    python3 (>= 3.10), python3-pyqt6, python3-pyqt6.qtsvg,
                python3-pyqtgraph, python3-matplotlib, python3-numpy,
                python3-yaml, python3-requests
    Recommends: python3-skyfield, python3-fastapi, python3-uvicorn

Uninstall:

    sudo apt-get remove --purge visible-ephemeris

---

## 3 · Source install (contributors)

    git clone https://github.com/n4hy/VisibleEphemerisCPP.git
    cd VisibleEphemerisCPP
    python3 -m venv venv
    ./venv/bin/pip install -r python_tracker/requirements.txt

That single `pip install` now pulls **everything** the tracker and the three
Qt dashboards need — `PyQt6` (with its bundled `PyQt6-Qt6` runtime, currently
Qt 6.11), `pyqtgraph`, `matplotlib`, `skyfield`, `fastapi`, `uvicorn`, `numpy`,
`PyYAML`, `requests`, and `pybind11`. Pip's self-contained PyQt6 wheels ship
their own Qt so they coexist happily with any system Qt.

If you'd rather use the system PyQt6/pyqtgraph (for smaller install size or
distro-integrated updates), pass `--system-site-packages` when creating the
venv and `apt install python3-pyqt6 python3-pyqtgraph python3-matplotlib`
first — the tracker will pick up whichever set is highest on `sys.path`.

Run without installing:

    ./venv/bin/python3 -m python_tracker.qt_dashboards.ve_ide
    ./venv/bin/python3 -m python_tracker.qt_dashboards.orbital_architect_qt
    ./venv/bin/python3 -m python_tracker.qt_dashboards.hpop_panel
    ./venv/bin/python3 -m python_tracker.main

Or, from a checkout, the tree-relative launchers work too — they auto-detect
the repo `venv/`, `.venv/`, or `env/` on the way out:

    ./bin/ve-ide
    ./bin/ve-orbital-architect-qt
    ./bin/ve-hpop-panel

`ve-ide` also detects and uses the project venv Python for its own Run
buttons, so ▶ Run tracker / ▶ Run tracker with --hpop pick up `skyfield`,
`fastapi`, and the rest automatically. The detected interpreter is printed in
the Run panel so you can confirm which one will be used.

---

## Verifying an install

    QT_QPA_PLATFORM=offscreen python3 -c \
      'import python_tracker.qt_dashboards.ve_ide, \
              python_tracker.qt_dashboards.orbital_architect_qt, \
              python_tracker.qt_dashboards.hpop_panel; print("ok")'

    which ve-ide ve-orbital-architect-qt ve-hpop-panel ve-tracker
    python3 -c "from PyQt6.QtCore import QT_VERSION_STR; print('Qt', QT_VERSION_STR)"

Expected: `ok`, four launcher paths, and a Qt version ≥ 6.4.

---

## Uninstall summary

| Install type   | Uninstall command                                  |
| -------------- | -------------------------------------------------- |
| `install.sh`   | `./install.sh --uninstall` (add `--system` if used) |
| `.deb`         | `sudo apt-get remove --purge visible-ephemeris`    |
| Source / venv  | `rm -rf .venv/`                                    |

---

## Optional: OD subsystem dependencies

> Skip this section unless you are building the orbit-determination (OD)
> subsystem — `cmake .. -DBUILD_OD=ON`. Nothing in the tracker, HPOP, or the
> Qt dashboards requires these libraries.
>
> **Status (v0, 2026-08-02):** The OD subsystem is implemented and builds;
> its unit-test suite (4 executables, 10 assertions) passes on the reference
> Ubuntu 24.04 + GCC 13 + x86_64 host. See
> [`orbit_determination.md`](orbit_determination.md) §14 for the file:line
> implementation index and §15 for measured behavior. Kilometre-scale RMS on
> synthetic benchmarks is a documented precision-floor finding tied to NLF's
> single-precision SRUKF, not a build/config issue.

The OD subsystem consumes two sibling repositories checked out next to this
one (all under `$HOME/` in the reference layout):

    ~/VisibleEphemerisCPP/                          # this repo
    ~/Modern-Computational-Nonlinear-Filtering/     # SRUKF + SRUKF smoothers
    ~/OptimizedKernelsForRaspberryPi5_NvidiaCUDA/   # NEON/SVE2/CUDA/Vulkan BLAS

Both are MIT-licensed and can be built and installed with CMake:

```bash
# 1. OptMathKernels (NEON/SVE2/cuBLAS/cuSOLVER/Vulkan dispatch layer)
cd ~/OptimizedKernelsForRaspberryPi5_NvidiaCUDA
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build        # installs OptMathKernels::OptMathKernels

# 2. NLF (Modern-Computational-Nonlinear-Filtering)
cd ~/Modern-Computational-Nonlinear-Filtering
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build        # installs nlf::nlf (find_package config)
```

`Eigen3 ≥ 3.4` is required by both siblings. On Debian/Ubuntu:

```bash
sudo apt-get install -y libeigen3-dev
```

Once both are installed, this repo's `cmake` picks them up via
`find_package(nlf REQUIRED)` and `find_package(OptMathKernels REQUIRED)`:

```bash
cd ~/VisibleEphemerisCPP
cmake -B build_od -S . \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DBUILD_OD=ON -DBUILD_TESTS=ON -DBUILD_OD_BENCHMARKS=ON
cmake --build build_od -j
ctest --test-dir build_od -R "^od_" --output-on-failure
```

**Why the explicit `gcc`/`g++` flags?** NLF's Config file requires OpenMP
via `find_dependency(OpenMP)`. Ubuntu ships GCC with `libgomp` built in,
so GCC works out of the box; the default Ubuntu Clang install does *not*
ship libomp headers. If you prefer Clang, install `libomp-dev` first.

Backend auto-selection:

| Host                                          | Accelerated path chosen at run time |
| --------------------------------------------- | ----------------------------------- |
| Raspberry Pi 5 (Cortex-A76)                   | NEON                                |
| Orange Pi 6+ (Cortex-A720, SVE2 / FCMA / I8MM)| SVE2                                |
| x86 + NVIDIA GPU                              | cuBLAS + cuSOLVER (Cholesky, GEMM)  |
| Any host with Vulkan 1.2+                     | Vulkan compute (fallback path)      |
| Any host, no accelerator                      | Eigen (portable C++20 fallback)     |

The Eigen fallback is bit-identical across hosts by design; the accelerated
paths match Eigen output to the tolerance documented in NLF's own tests. No
runtime configuration is required — the fastest available path is chosen at
compile time via CMake option flags in `OptMathKernels`.

---

## Non-Debian Linux

`install.sh` still works on non-Debian systems if you pass `--no-apt` and
install the following via your package manager first:

    python3-pyqt6  python3-pyqtgraph  python3-matplotlib
    python3-numpy  python3-pyyaml     python3-requests
    # optional, but strongly recommended
    python3-skyfield  python3-fastapi  python3-uvicorn

Fedora / RHEL:  `sudo dnf install python3-qt6 python3-pyqtgraph …`
Arch:           `sudo pacman -S python-pyqt6 python-pyqtgraph …`
openSUSE:       `sudo zypper install python3-qt6 python3-pyqtgraph …`

Then run `./install.sh --no-apt --user` and follow the usual instructions.
