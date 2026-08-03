#!/usr/bin/env bash
# run_nlf_reentry.sh -- Benchmark B4: cross-check against NLF's own SRUKF
# reentry-vehicle benchmark.
#
# We do not duplicate the NLF reentry benchmark in this repository. Instead we
# run the binary that ships with NLF and (in the future) diff its output CSV
# against a golden reference to certify that our INTEGRATION of NLF is not
# the source of discrepancies observed in bench_synthetic (B1, B2).
#
# Newton discipline: this is a certification of the library dependency, not a
# claim about our OD subsystem. Failure here means "check NLF"; success here
# means "any B1/B2 discrepancy is on us."

set -euo pipefail

NLF_ROOT="${NLF_ROOT:-$HOME/Modern-Computational-Nonlinear-Filtering}"
NLF_BIN="$NLF_ROOT/install/bin/run_benchmarks"

if [ ! -x "$NLF_BIN" ]; then
    echo "run_benchmarks not found at $NLF_BIN"
    echo "Set NLF_ROOT env var to the NLF checkout root, or install NLF first:"
    echo "  cmake -B build -S \$NLF_ROOT && cmake --build build -j && sudo cmake --install build"
    exit 2
fi

OUTDIR="${1:-./nlf_reentry_output}"
mkdir -p "$OUTDIR"
cd "$OUTDIR"

echo "Running $NLF_BIN in $(pwd)..."
"$NLF_BIN"
echo ""
echo "NLF benchmark outputs written to $(pwd):"
ls -la *reentry*.csv 2>/dev/null || echo "(no reentry CSVs produced)"
echo ""
echo "Newton note: this certifies NLF's own reentry test still runs to"
echo "completion; a diff against a golden CSV is a future addition."
