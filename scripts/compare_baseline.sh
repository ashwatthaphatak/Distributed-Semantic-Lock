#!/usr/bin/env bash
# Run the DSLM and Qdrant-direct Locust workloads back-to-back against the
# same traffic shape, then produce a side-by-side overhead plot + summary CSV.
#
# The two runs share every workload knob (personas, interval, jitter, payload
# selection) so the difference between them is attributable to the DSLM
# coordination stack (proxy + leader hop + Raft + lock table + lock hold).
#
# Outputs (written to $OUTPUT_DIR):
#   baseline_stats.csv, baseline_stats_history.csv, baseline.log
#   dslm_stats.csv,     dslm_stats_history.csv,     dslm.log
#   overhead_comparison.png
#   overhead_summary.csv
#
# Usage:
#   scripts/compare_baseline.sh [-d DURATION] [-u USERS] [-r RATE] [-o OUTPUT_DIR]
#
# See -h for details.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ----- Defaults -------------------------------------------------------------

DURATION="${DURATION:-5m}"
USERS="${USERS:-5}"
SPAWN_RATE="${SPAWN_RATE:-5}"
OUTPUT_DIR="${OUTPUT_DIR:-${REPO_ROOT}/logs/overhead_$(date +%Y%m%d_%H%M%S)}"
SKIP_PLOT="${SKIP_PLOT:-0}"

# Web-UI knobs.  With WATCH=1, each locust run keeps its real-time web UI
# alive (http://<WEB_HOST>:<WEB_PORT>) instead of running fully headless.
# AUTOQUIT_SEC controls how long the UI stays up after the run finishes so
# you can inspect the final charts before the next run starts.
WATCH="${WATCH:-0}"
WEB_HOST="${WEB_HOST:-0.0.0.0}"
WEB_PORT="${WEB_PORT:-8089}"
AUTOQUIT_SEC="${AUTOQUIT_SEC:-15}"

# Workload shape — identical between the two runs.
export DSCC_AGENT_LABELS="${DSCC_AGENT_LABELS:-ABCDE}"
export DSCC_OP_INTERVAL_MS="${DSCC_OP_INTERVAL_MS:-1000}"
export DSCC_OP_JITTER_MS="${DSCC_OP_JITTER_MS:-200}"
export DSCC_USE_FIRST_PAYLOAD_ONLY="${DSCC_USE_FIRST_PAYLOAD_ONLY:-1}"

# Endpoints (override via env when the client runs on a different machine).
export DSCC_PROXY="${DSCC_PROXY:-localhost:50050}"
export QDRANT_HOST="${QDRANT_HOST:-localhost}"
export QDRANT_PORT="${QDRANT_PORT:-6333}"
export EMBEDDING_HOST="${EMBEDDING_HOST:-localhost}"
export EMBEDDING_PORT="${EMBEDDING_PORT:-7997}"
export EMBEDDING_MODEL="${EMBEDDING_MODEL:-all-minilm:latest}"

# Baseline-only knobs.
export QDRANT_COLLECTION="${QDRANT_COLLECTION:-dscc_baseline}"
export BASELINE_RESET_COLLECTION="${BASELINE_RESET_COLLECTION:-1}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [-d DURATION] [-u USERS] [-r RATE] [-o OUTPUT_DIR]
                        [-w] [-p WEB_PORT] [-P]

Runs both workloads sequentially:
  1. locustfile_base.py  — raw Qdrant HTTP traffic (baseline)
  2. locustfile.py       — full DSLM gRPC traffic

Options:
  -d DURATION     Run time for EACH locust run      (default: ${DURATION})
  -u USERS        Concurrent virtual users          (default: ${USERS})
  -r RATE         Spawn rate (users/sec)            (default: ${SPAWN_RATE})
  -o OUTPUT_DIR   Where CSVs and plots land         (default: logs/overhead_<ts>)
  -w              WATCH mode — keep Locust's live web UI up during each run
                  (http://localhost:\${WEB_PORT}).  The test still autostarts
                  and autoquits after --run-time + AUTOQUIT_SEC grace period.
  -p WEB_PORT     Port for the live web UI           (default: ${WEB_PORT})
  -P              Skip plot generation (CSVs only)
  -h              Show this help

Environment overrides:
  DSCC_PROXY                    Proxy target            (default: localhost:50050)
  QDRANT_HOST / QDRANT_PORT     Qdrant HTTP endpoint    (default: localhost:6333)
  QDRANT_COLLECTION             Baseline collection     (default: dscc_baseline)
  BASELINE_RESET_COLLECTION     1 = reset on startup    (default: 1)
  EMBEDDING_HOST / _PORT        Embedding service       (default: localhost:7997)
  EMBEDDING_MODEL               Model name              (default: all-minilm:latest)
  DSCC_AGENT_LABELS             Persona filter          (default: ABCDE)
  DSCC_OP_INTERVAL_MS           Per-user interval       (default: 1000)
  DSCC_OP_JITTER_MS             Interval jitter         (default: 200)
  DSCC_USE_FIRST_PAYLOAD_ONLY   1 = first payload only  (default: 1)

For a fair coordination-cost measurement, restart the DSLM stack with
DSCC_LOCK_HOLD_MS=0 before running this script — otherwise the DSLM numbers
include ~750 ms of configured lock-hold time that is not real overhead.

Example (client on a different host):
  DSCC_PROXY=10.0.0.5:50050 QDRANT_HOST=10.0.0.5 \\
      scripts/compare_baseline.sh -d 2m -u 5 -r 5
EOF
}

while getopts "d:u:r:o:p:wPh" opt; do
    case "$opt" in
        d) DURATION="$OPTARG" ;;
        u) USERS="$OPTARG" ;;
        r) SPAWN_RATE="$OPTARG" ;;
        o) OUTPUT_DIR="$OPTARG" ;;
        p) WEB_PORT="$OPTARG" ;;
        w) WATCH=1 ;;
        P) SKIP_PLOT=1 ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

mkdir -p "${OUTPUT_DIR}"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }

# ----- Sanity checks --------------------------------------------------------

if ! command -v locust >/dev/null 2>&1; then
    echo "locust not found on PATH (pip install locust)" >&2
    exit 1
fi

# Pick a Python interpreter that has matplotlib + numpy for the plot step.
# On macOS it's common to have `python3` resolve to a Homebrew install without
# those packages while `pip install matplotlib` landed in Python.framework, so
# we probe candidates in order and fall back to plain `python3` if nothing
# has matplotlib (the plot script still writes the CSV in that case).
select_python() {
    local candidates=()
    [[ -n "${PYTHON:-}" ]] && candidates+=("${PYTHON}")
    candidates+=(python3.13 python3.12 python3.11 python3.10 python3 python)
    for py in "${candidates[@]}"; do
        if command -v "$py" >/dev/null 2>&1 \
            && "$py" -c "import matplotlib, numpy" >/dev/null 2>&1; then
            echo "$py"
            return 0
        fi
    done
    command -v python3 >/dev/null 2>&1 && echo python3 && return 0
    command -v python  >/dev/null 2>&1 && echo python  && return 0
    echo "no python interpreter found on PATH" >&2
    exit 1
}
PYTHON_BIN="$(select_python)"

# ----- Run --------------------------------------------------------------------

log "Output directory: ${OUTPUT_DIR}"
log "Duration:         ${DURATION}   Users: ${USERS}   Spawn rate: ${SPAWN_RATE}"
log "DSCC_PROXY:       ${DSCC_PROXY}"
log "QDRANT:           ${QDRANT_HOST}:${QDRANT_PORT} (baseline collection=${QDRANT_COLLECTION})"
log "EMBEDDING:        ${EMBEDDING_HOST}:${EMBEDDING_PORT} (model=${EMBEDDING_MODEL})"
log "Workload:         labels=${DSCC_AGENT_LABELS} interval=${DSCC_OP_INTERVAL_MS}ms jitter=${DSCC_OP_JITTER_MS}ms"
log "Python (plot):    ${PYTHON_BIN} ($("${PYTHON_BIN}" --version 2>&1))"

cd "${REPO_ROOT}"

# Build the mode-specific locust flags once so both runs share them.
# --autostart keeps the web UI alive while starting the test automatically.
# --autoquit shuts Locust down N seconds after the run-time elapses, so the
# compare script can flow from baseline → DSLM without user interaction.
if [[ "${WATCH}" == "1" ]]; then
    MODE_FLAGS=(--autostart
                --autoquit "${AUTOQUIT_SEC}"
                --web-host "${WEB_HOST}"
                --web-port "${WEB_PORT}")
    log "WATCH mode ON — open http://localhost:${WEB_PORT} during each run for live charts (grace=${AUTOQUIT_SEC}s)"
else
    MODE_FLAGS=(--headless --only-summary)
fi

log "Run 1/2: BASELINE — direct Qdrant traffic"
locust -f locustfile_base.py \
    "${MODE_FLAGS[@]}" \
    -u "${USERS}" -r "${SPAWN_RATE}" \
    --run-time "${DURATION}" \
    --csv "${OUTPUT_DIR}/baseline" \
    2>&1 | tee "${OUTPUT_DIR}/baseline.log"

log "Run 2/2: DSLM — full coordination stack"
locust -f locustfile.py \
    "${MODE_FLAGS[@]}" \
    -u "${USERS}" -r "${SPAWN_RATE}" \
    --run-time "${DURATION}" \
    --csv "${OUTPUT_DIR}/dslm" \
    2>&1 | tee "${OUTPUT_DIR}/dslm.log"

# ----- Plot -----------------------------------------------------------------

if [[ "${SKIP_PLOT}" == "1" ]]; then
    log "Skipping plot generation (-P)."
else
    log "Generating overhead comparison plot"
    if ! "${PYTHON_BIN}" "${SCRIPT_DIR}/plot_overhead.py" \
            --baseline "${OUTPUT_DIR}/baseline_stats.csv" \
            --dslm     "${OUTPUT_DIR}/dslm_stats.csv" \
            --output-dir "${OUTPUT_DIR}"; then
        warn "Plot step failed. CSVs are still available in ${OUTPUT_DIR}."
    fi
fi

# ----- Summary --------------------------------------------------------------

log "Done."
cat <<EOF

Artifacts in ${OUTPUT_DIR}:
  baseline_stats.csv           — per-endpoint stats from the direct-Qdrant run
  dslm_stats.csv               — per-endpoint stats from the full DSLM run
  baseline.log / dslm.log      — full locust headless output
  overhead_comparison.png      — grouped + delta bar chart (if plot step ran)
  overhead_summary.csv         — matched side-by-side latency numbers
EOF
