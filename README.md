# DSCC / DSLM

Distributed semantic lock manager in front of Qdrant.

The system accepts natural-language payloads plus embedding vectors and blocks requests whose cosine similarity to an active semantic lock is greater than or equal to a configured threshold `theta`.

## Architecture

Current end-to-end request path:

```text
demo_inputs/*.json
    -> embedding-service
    -> dscc-proxy
    -> current dscc-node leader
    -> ActiveLockTable
    -> Raft ACQUIRE replication
    -> Qdrant read/write
    -> Raft RELEASE replication
```

Main runtime pieces:

- `dscc-node`
  - lock service + Raft service on each node
- `dscc-proxy`
  - leader-aware forwarding layer for client traffic
- `embedding-service`
  - embedding generation for demo and benchmark inputs
- `qdrant`
  - vector storage and vector search

Main source files:

- [src/active_lock_table.h](/home/ubuntu/cpp_projects/DSLM/src/active_lock_table.h)
- [src/active_lock_table.cpp](/home/ubuntu/cpp_projects/DSLM/src/active_lock_table.cpp)
- [src/lock_service_impl.h](/home/ubuntu/cpp_projects/DSLM/src/lock_service_impl.h)
- [src/lock_service_impl.cpp](/home/ubuntu/cpp_projects/DSLM/src/lock_service_impl.cpp)
- [src/proxy_service_impl.h](/home/ubuntu/cpp_projects/DSLM/src/proxy_service_impl.h)
- [src/proxy_service_impl.cpp](/home/ubuntu/cpp_projects/DSLM/src/proxy_service_impl.cpp)
- [src/raft_node.h](/home/ubuntu/cpp_projects/DSLM/src/raft_node.h)
- [src/raft_node.cpp](/home/ubuntu/cpp_projects/DSLM/src/raft_node.cpp)
- [src/e2e_bench.cpp](/home/ubuntu/cpp_projects/DSLM/src/e2e_bench.cpp)
- [src/benchmark_runner.cpp](/home/ubuntu/cpp_projects/DSLM/src/benchmark_runner.cpp)

For a detailed current-state description, see [STATE.md](/home/ubuntu/cpp_projects/DSLM/STATE.md).

## Build

Configure once:

```bash
cmake -S . -B /tmp/dslm_build
```

Build the main targets:

```bash
cmake --build /tmp/dslm_build --target dscc-node -j"$(nproc)"
cmake --build /tmp/dslm_build --target dscc-proxy -j"$(nproc)"
cmake --build /tmp/dslm_build --target dscc-e2e-bench -j"$(nproc)"
cmake --build /tmp/dslm_build --target dscc-benchmark -j"$(nproc)"
```

Useful extra targets:

```bash
cmake --build /tmp/dslm_build --target dscc-testbench -j"$(nproc)"
cmake --build /tmp/dslm_build --target dscc-raft-test -j"$(nproc)"
```

## Demo Inputs

Current seeded inputs live in:

- [demo_inputs/A.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/A.json)
- [demo_inputs/B.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/B.json)
- [demo_inputs/C.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/C.json)
- [demo_inputs/D.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/D.json)
- [demo_inputs/E.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/E.json)

Each file supports:

- `payload`
- `scheduled_offset_ms`
- `operation`
- optional `payload_schedule`

When `payload_schedule` is present, both the e2e harness and the curated benchmark consume the first scheduled entry for that agent/template.

## End-to-End Demo

Run the full demo harness:

```bash
/tmp/dslm_build/dscc-e2e-bench
```

Run with automatic teardown:

```bash
E2E_TEARDOWN=1 /tmp/dslm_build/dscc-e2e-bench
```

Useful environment variables:

- `DSCC_THETA`
- `DSCC_LOCK_HOLD_MS`
- `EMBEDDING_IMAGE`
- `EMBEDDING_MODEL_ID`
- `QDRANT_COLLECTION`
- `E2E_TEARDOWN`

Examples:

```bash
DSCC_THETA=0.55 /tmp/dslm_build/dscc-e2e-bench
DSCC_THETA=0.90 DSCC_LOCK_HOLD_MS=1000 /tmp/dslm_build/dscc-e2e-bench
```

## Curated Benchmark Runner

`dscc-benchmark` runs three experiment modes against the live Docker stack.
Build it once, then choose a mode below.

```bash
cmake --build /tmp/dslm_build --target dscc-benchmark -j"$(nproc)"
```

---

### Single mode (default)

Runs 13 curated scenarios in sequence. Each scenario uses its own per-case
similarity threshold; the stack is torn down and recreated between cases.

```bash
E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

Output: `logs/benchmark_run_<timestamp>.json`

Override the output path:

```bash
DSLM_BENCH_OUTPUT=/tmp/myrun.json E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

---

### Matrix mode

Sweeps all three embedding models across three similarity thresholds
(θ = 0.55, 0.75, 0.95), running all 13 scenarios for every combination
(3 × 3 × 13 = 117 case runs total). Results accumulate in one shared CSV file
and one JSON file per (model, θ) pair.

**Full sweep (all three models):**

```bash
DSLM_RUN_MODE=matrix E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

Output: `logs/benchmark_run_<timestamp>_matrix.csv` plus per-combination JSON files.

**Run a single model only** (useful for resuming after a crash):

```bash
# only bge-m3
DSLM_RUN_MODE=matrix DSLM_MATRIX_PROFILE=bge E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark

# only qwen3-embedding
DSLM_RUN_MODE=matrix DSLM_MATRIX_PROFILE=qwen E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

**Run a single model at a single threshold** (fill in missing rows after a crash):

```bash
DSLM_RUN_MODE=matrix DSLM_MATRIX_PROFILE=bge DSLM_MATRIX_THETA=0.95 \
  DSLM_MATRIX_OUTPUT=logs/existing_matrix.csv \
  E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

`DSLM_MATRIX_OUTPUT` appends to an existing CSV file if it already exists.
The plotting script deduplicates rows by (profile, θ, scenario) automatically,
so it is safe to re-run any combination and append to the same file.

**Matrix environment variables:**

| Variable | Default | Description |
|---|---|---|
| `DSLM_RUN_MODE` | `single` | Set to `matrix` to enable sweep |
| `DSLM_MATRIX_PROFILE` | *(all three)* | Restrict to `ollama`, `bge`, or `qwen` |
| `DSLM_MATRIX_THETA` | *(all three)* | Restrict to a single θ value, e.g. `0.75` |
| `DSLM_MATRIX_OUTPUT` | auto-generated | Explicit path for the shared CSV output |

---

### Soak mode

Keeps the stack running for an extended period and streams a growing Qdrant
collection, taking windowed latency snapshots every 60 seconds. Use this to
verify that lock-wait latency stays flat as the vector DB fills up.

**Default run (2 hours):**

```bash
DSLM_RUN_MODE=soak E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

**Short run for a quick sanity check:**

```bash
DSLM_RUN_MODE=soak DSLM_SOAK_DURATION_MIN=30 E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

**Adjust the snapshot interval and similarity threshold:**

```bash
DSLM_RUN_MODE=soak \
  DSLM_SOAK_DURATION_MIN=120 \
  DSLM_SOAK_SNAPSHOT_SEC=60 \
  DSLM_SOAK_THETA=0.75 \
  DSLM_SOAK_LOCK_HOLD_MS=500 \
  E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

Output: `logs/soak_run_<timestamp>.csv`

**Soak environment variables:**

| Variable | Default | Description |
|---|---|---|
| `DSLM_SOAK_DURATION_MIN` | `120` | Total run time in minutes |
| `DSLM_SOAK_SNAPSHOT_SEC` | `60` | How often to write a CSV snapshot row |
| `DSLM_SOAK_THETA` | `0.75` | Similarity threshold for the soak workload |
| `DSLM_SOAK_LOCK_HOLD_MS` | `500` | Lock hold duration per operation |
| `DSLM_SOAK_OUTPUT` | auto-generated | Explicit path for the time-series CSV |

---

### Scenario set (all modes)

1. The Thundering Herd
2. The Semantic Interleaving
3. The Read-Starvation Trap
4. The Permissive Sieve
5. The Strict Sieve
6. The Ghost Client
7. The Almost Collision
8. Queue Hopping
9. The Mixed Stagger
10. The 100% Read Stampede
11. The Paraphrase Gauntlet *(research: paraphrase detection across models)*
12. The Cross-Domain Flood *(research: specificity across unrelated domains)*
13. The Write Pressure Ratchet *(research: reader fairness under write load)*

---

## Plotting

Install dependencies once:

```bash
pip install matplotlib numpy pandas
```

**Matrix sweep — model comparison plots:**

```bash
# auto-detect the latest *_matrix.csv in logs/
python3 scripts/plot_matrix_metrics.py

# explicit file
python3 scripts/plot_matrix_metrics.py logs/benchmark_run_<timestamp>_matrix.csv
```

Writes 8 PNG files to `scripts/plots/matrix/`.

**Soak test — latency-over-time plots:**

```bash
# auto-detect the latest soak_run_*.csv in logs/
python3 scripts/plot_soak_test.py

# explicit file
python3 scripts/plot_soak_test.py logs/soak_run_<timestamp>.csv
```

Writes 7 PNG files to `scripts/plots/soak/`.

**Single-run timing report:**

```bash
python3 scripts/plot_benchmark_report.py logs/benchmark_run_<timestamp>.json
```

**Multi-model timing comparison:**

```bash
python3 scripts/plot_model_comparison.py logs/benchmark_run_*.json
```

---

### Benchmark output format

**Live queue events** printed during each case:

```text
[LOCK_QUEUE]   agent=sustainability_agent_4 waiting_on=sustainability_agent_1 similarity=1.000 queue_position=3 theta=0.550
[LOCK_REQUEUE] agent=sustainability_agent_4 waiting_on=sustainability_agent_2 similarity=1.000 queue_position=2 queue_hops=1 theta=0.550
[LOCK_GRANT]   agent=sustainability_agent_4 queue_hops=1 active_locks=1
```

**Correctness check:** any pair of operations with pairwise similarity ≥ θ must
not overlap in their active lock intervals. A violation means two conflicting
requests were both in the critical section at the same time. Violations are
printed inline and counted in `conflicting_overlap_violations` in the JSON output.

See [MODEL_FINDINGS.md](MODEL_FINDINGS.md) for metric definitions and experimental
results across the three embedding models.

## Reading the Benchmark Output

### Live queue events

During a case you will now see lines such as:

```text
[LOCK_QUEUE] agent=sustainability_agent_4 waiting_on=sustainability_agent_1 similarity=1.000 queue_position=3 theta=0.550
[LOCK_REQUEUE] agent=sustainability_agent_4 waiting_on=sustainability_agent_2 similarity=1.000 queue_position=2 queue_hops=1 theta=0.550
[LOCK_GRANT] agent=sustainability_agent_4 queue_hops=1 active_locks=1
```

These come directly from the active lock table and show why a request entered or re-entered a queue.

### Timeline

The per-case timeline shows:

- when a request blocked
- who blocked it
- the similarity that caused the block
- when it was granted
- queue hops
- when it released
- explicit correctness-violation events when two conflicting lock intervals overlap

### Correctness

Benchmark correctness means:

- if two requests have pairwise similarity `>= theta`, they should not overlap in the active critical section

A violation means:

- two conflicting requests were both active at the same time according to the recorded lock-acquire and lock-release timestamps

## Docker

Start the stack manually:

```bash
docker compose up -d --build qdrant embedding-service dscc-node-1 dscc-node-2 dscc-node-3 dscc-node-4 dscc-node-5 dscc-proxy
```

Stop it:

```bash
docker compose down
```

## Notes

- CMake generates protobuf/gRPC code in the build directory.
- Generated protobuf files should stay out of the source tree.
- `STATE.md` is the authoritative current-state document for this repository.
