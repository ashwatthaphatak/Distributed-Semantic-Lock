# scripts — Plotting and Analysis Tools

**Authors: Ashwattha Phatak (matrix/model/overhead plots), Ayush Gala (benchmark report, soak, baseline comparison, workload generators)**  
CSC 724 — Advanced Distributed Systems, NC State University

---

## Directory layout

```
scripts/
├── workload/       Workload generators — produce logs and benchmark data
│   ├── locustfile.py           Full DSLM gRPC load generator (Locust)
│   ├── locustfile_base.py      Qdrant-direct baseline load generator (Locust)
│   ├── thundering_herd.py      Standalone thundering-herd burst generator
│   ├── agent_request.sh        Single-agent request helper (grpcurl)
│   └── compare_baseline.sh     Runs both Locust workloads back-to-back + plots
│
├── analysis/       Log processors — read logs/, write plots/
│   ├── plot_matrix_metrics.py      Model × theta matrix comparison (Ashwattha Phatak)
│   ├── plot_model_comparison.py    Timing comparison across models (Ashwattha Phatak)
│   ├── plot_overhead.py            Lock-admission overhead analysis (Ashwattha Phatak)
│   ├── plot_benchmark_report.py    Single-run timing report (Ayush Gala)
│   └── plot_soak_test.py           Soak-test latency-over-time plots (Ayush Gala)
│
└── plots/          Generated PNG output (gitignored — run scripts to regenerate)
    ├── matrix/
    ├── model_comparison/
    └── soak/
```

---

## Prerequisites

```bash
pip install matplotlib numpy pandas
pip install locust grpcio grpcio-tools requests   # for workload generators only
```

---

## Workload generators (`scripts/workload/`)

### `locustfile.py` — full DSLM gRPC load (Ayush Gala)

Fires `AcquireGuard` requests through the DSCC proxy using agents A–E (the overlap-dense subset). Mirrors the e2e_demo.cpp thread loop pacing.

```bash
# Web UI (http://localhost:8089)
locust -f scripts/workload/locustfile.py

# Headless — 5 users, 5/s ramp, 5 min
locust -f scripts/workload/locustfile.py --headless -u 5 -r 5 --run-time 5m
```

Requires proto stubs generated at the project root:
```bash
python -m grpc_tools.protoc -I proto --python_out=. --grpc_python_out=. proto/dscc.proto
```

---

### `locustfile_base.py` — Qdrant-direct baseline (Ayush Gala)

Same personas and pacing as `locustfile.py` but routes each op straight to Qdrant over HTTP. Use alongside `locustfile.py` to measure coordination overhead.

```bash
locust -f scripts/workload/locustfile_base.py --headless -u 5 -r 5 --run-time 5m
```

---

### `thundering_herd.py` — burst generator (Ayush Gala)

Launches `HERD_AGENTS` (default 10) threads that all fire simultaneously, then waits and repeats for `HERD_WAVES` (default 5) waves. Mirrors benchmark scenario #1 exactly.

```bash
python3 scripts/workload/thundering_herd.py

# Remote via Tailscale:
DSCC_PROXY=<IP>:50050 EMBEDDING_HOST=<IP> python3 scripts/workload/thundering_herd.py
```

---

### `agent_request.sh` — single-agent helper (Ayush Gala)

Converts a plain-text payload to an embedding and sends one `AcquireGuard` gRPC request.

```bash
bash scripts/workload/agent_request.sh write "Review the massing concept"
bash scripts/workload/agent_request.sh read  "Check daylight metrics for the west facade"
```

---

### `compare_baseline.sh` — overhead comparison (Ayush Gala)

Runs the baseline and DSLM Locust workloads back-to-back and generates a side-by-side overhead plot.

```bash
bash scripts/workload/compare_baseline.sh
bash scripts/workload/compare_baseline.sh -d 2m -u 5 -r 5
```

Output lands in `logs/overhead_<timestamp>/`.

---

## Analysis scripts (`scripts/analysis/`)

### `plot_matrix_metrics.py` — model comparison across theta sweep (Ashwattha Phatak)

Reads a `*_matrix.csv` from `dscc-benchmark` matrix mode and writes 8 PNG files to `scripts/plots/matrix/`.

```bash
python3 scripts/analysis/plot_matrix_metrics.py
python3 scripts/analysis/plot_matrix_metrics.py logs/benchmark_run_<timestamp>_matrix.csv
```

---

### `plot_model_comparison.py` — timing across benchmark runs (Ashwattha Phatak)

Compares multiple single-run JSON files and writes comparison plots to `scripts/plots/model_comparison/`.

```bash
python3 scripts/analysis/plot_model_comparison.py logs/benchmark_run_*.json
```

---

### `plot_overhead.py` — lock-admission overhead analysis (Ashwattha Phatak)

Reads paired Locust CSV files (baseline + DSLM) and produces a grouped overhead bar chart.

```bash
python3 scripts/analysis/plot_overhead.py \
    --baseline logs/overhead_.../baseline_stats.csv \
    --dslm     logs/overhead_.../dslm_stats.csv \
    --output-dir logs/overhead_.../
```

---

### `plot_benchmark_report.py` — single-run timing report (Ayush Gala)

```bash
python3 scripts/analysis/plot_benchmark_report.py logs/benchmark_run_<timestamp>.json
```

---

### `plot_soak_test.py` — soak-test latency over time (Ayush Gala)

```bash
python3 scripts/analysis/plot_soak_test.py
python3 scripts/analysis/plot_soak_test.py logs/soak_run_<timestamp>.csv
```

Writes 7 PNG files to `scripts/plots/soak/`.

---

## Generated output

All PNG files (`scripts/plots/**/*.png`) are gitignored. Run the analysis scripts above after producing the corresponding log files to regenerate them.
