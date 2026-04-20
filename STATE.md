# DSLM / DSCC Current State

## Context for AI Models

This document is the current-state companion to `expl_docs/VERSION_0.md`.
It is intentionally focused on what exists in the repository now, how the current distributed runtime actually behaves, what the benchmark harness measures, and which architectural caveats are still unresolved.

Use this file when you need an up-to-date mental model of the repo before making changes.

---

## 1. What This Repository Is Right Now

This repository implements a distributed semantic lock manager in front of Qdrant.

Core idea:

- every request carries natural-language payload text plus its embedding
- the leader compares that embedding against the embeddings of currently active semantic locks
- if cosine similarity is greater than or equal to `theta`, the request waits
- if cosine similarity is below `theta`, the request may proceed in parallel

The current system is no longer just a single-node prototype.
It now includes:

- a multi-node DSCC runtime with leader election and log replication via Raft
- a leader-aware proxy in front of the node cluster
- a queueing semantic lock table with requeue and hop tracking
- both `write` and `read` operations through the same semantic admission path
- an end-to-end demo harness (`dscc-e2e-bench`)
- a curated benchmark runner (`dscc-benchmark`) with three modes:
  - **single** — 13 scenarios in sequence, each with its own per-case theta
  - **matrix** — 3 embedding models × 3 thetas × 13 scenarios (117 case runs), outputs a shared CSV
  - **soak** — continuous insertion for a configurable duration (default 2 hours), windowed latency snapshots every 60 s to measure latency stability as Qdrant collection grows
- an embedding model evaluation across `all-minilm:latest`, `bge-m3:latest`, and `qwen3-embedding:0.6b` (see `MODEL_FINDINGS.md`)

At the same time, the system is still a research prototype.
Some benchmark scenarios still report correctness violations, so the repo should be treated as operational but not yet behaviorally closed.

---

## 2. Current Scope and Important Caveats

### 2.1 What is implemented

- gRPC lock service exposed through `dscc-proxy`
- five `dscc-node-*` processes with embedded Raft and semantic lock service
- leader election and leader-aware request forwarding
- in-memory semantic lock table with per-lock waiter queues
- Qdrant upsert path for writes
- Qdrant vector-query path for reads
- benchmark traces for:
  - lock wait time
  - blocking agent id
  - blocking similarity
  - queue position
  - wake count
  - queue hops
  - active lock count
- curated benchmark runner with:
  - role-based agent names
  - live queue-event stream
  - timeline output
  - post-hoc correctness-violation markers
  - raw JSON output

### 2.2 What is still incomplete or risky

- semantic lock ownership is still primarily in-memory
- there is no durable snapshot/restore path for active lock state
- correctness violations still appear in some benchmark scenarios
- the benchmark correctness metric is interval-overlap based and depends on timestamp semantics
- there is no lease-based expiration or heartbeat-based cleanup for abandoned locks
- there is no persistent recovery story for active lock state after process restart
- the current read path is conservative because reads also go through semantic locking

### 2.3 Current leader path (pending/promote with Raft)

The current leader path is:

1. `wait_for_admission` — block until no semantic conflict, then insert a **pending** slot
2. `Propose(ACQUIRE)` via Raft — replicate to quorum
3. `WaitUntilApplied` — block until the apply callback promotes pending → real lock
4. perform Qdrant work (upsert for writes, search for reads)
5. `Propose(RELEASE)` via Raft — replicate and apply; release lock and rebalance waiters

The pending slot (step 1) participates in conflict detection: subsequent conflicting requests block behind it even before the Raft commit. This prevents any request from slipping through between admission and commit. No Qdrant operation happens until the lock is durably committed through Raft (step 3 completes before step 4 begins).

If `Propose` fails (quorum not reached), `remove_pending` cleans up the pending slot and a compensating `RELEASE` is appended via `AppendLocalEntry` to cancel any partially replicated ghost `ACQUIRE`.

This is the "Raft-first with pending/promote" model described in `expl_docs/VERSION_1.md` §7.2 (Option B). The system is Raft-authoritative before proceeding to Qdrant. The remaining correctness violations observed in benchmarks are likely related to timestamp-boundary effects in the overlap metric rather than a fundamental admission ordering issue.

---

## 3. Repository Structure

```
DSLM/
├── CMakeLists.txt
├── docker-compose.yml
├── docker/
│   └── Dockerfile
├── proto/
│   ├── dscc.proto
│   ├── dscc_raft.proto
│   └── generated protobuf / gRPC files under the build directory
├── src/
│   ├── main.cpp
│   ├── lock_service_impl.h / .cpp
│   ├── active_lock_table.h / .cpp
│   ├── raft_node.h / .cpp
│   ├── raft_service_impl.h / .cpp
│   ├── proxy_main.cpp
│   ├── proxy_service_impl.h / .cpp
│   ├── threadsafe_log.h / .cpp
│   ├── testbench.cpp
│   ├── e2e_bench.cpp
│   ├── benchmark_runner.cpp
│   └── raft_test.cpp
├── demo_inputs/
│   ├── A.json
│   ├── B.json
│   ├── C.json
│   ├── D.json
│   └── E.json
├── README.md
├── expl_docs/VERSION_0.md         # historical architecture snapshot (formerly CURRENT_SYSTEM.md)
├── STATE.md
└── logs/benchmark_run_<timestamp>.json
```

Main build targets currently present:

- `dscc-node`
- `dscc-proxy`
- `dscc-e2e-bench`
- `dscc-benchmark`
- `dscc-testbench`
- `dscc-raft-test`

---

## 4. Runtime Architecture Overview

### 4.1 Runtime components

The current full benchmark/demo stack has five logical parts:

```
agent workload
    |
    v
embedding-service  -> generates embeddings
    |
    v
dscc-proxy         -> forwards to current leader
    |
    v
dscc-node leader   -> semantic admission + Raft proposal + Qdrant operation
    |
    +--> follower replication via Raft
    |
    v
Qdrant             -> vector persistence / vector search
```

### 4.2 Components in practice

`embedding-service`
- currently used for embedding generation from the benchmark/demo harness

`dscc-proxy`
- C++ gRPC service (`src/proxy_main.cpp`, `src/proxy_service_impl.cpp/.h`), built by the same CMake toolchain and Dockerfile as the nodes (the original architecture plan proposed a Go proxy; the decision was made to keep the entire stack in C++ for build simplicity and a single container image)
- polls the cluster for leader information via `RaftService::GetLeader`
- forwards `AcquireGuard` and `ReleaseGuard` to the current leader
- retries on `NOT_LEADER`, `UNAVAILABLE`, and `DEADLINE_EXCEEDED`
- caches gRPC channels per backend address

`dscc-node-*`
- exposes both the client-facing lock service and the Raft service
- owns:
  - `ActiveLockTable`
  - `RaftNode`
  - `LockServiceImpl`
  - `RaftServiceImpl`

`Qdrant`
- stores points for writes
- serves vector search queries for reads

`benchmark_runner`
- starts and tears down the stack case by case
- builds curated workloads
- streams live queue events by polling node logs
- computes correctness and parallelism metrics
- writes `logs/benchmark_run_<timestamp>.json`

---

## 5. Current End-to-End Request Flow

The current distributed request path is:

1. client or benchmark sends `AcquireGuard` to `dscc-proxy`
2. proxy resolves or refreshes the current leader via `RaftService::GetLeader`
3. proxy forwards the RPC to the leader node
4. leader validates request fields (`agent_id`, `embedding` non-empty)
5. leader checks `raft_->IsLeader()`; if not leader, returns `FAILED_PRECONDITION` with `leader-address` trailing metadata
6. leader calls `ActiveLockTable::wait_for_admission` — blocks until no semantic conflict, then inserts a **pending** slot
7. leader calls `RaftNode::Propose(ACQUIRE)` — replicates to quorum and commits
8. leader calls `RaftNode::WaitUntilApplied` — blocks until the apply callback promotes pending → real lock on all replicas
9. leader performs Qdrant operation (the lock is now durably committed):
   - write: upsert vector and metadata into Qdrant
   - read: issue vector similarity query against Qdrant
10. leader calls `RaftNode::Propose(RELEASE)` and `WaitUntilApplied` — apply callback releases the lock and rebalances waiters
11. RPC returns timing and trace metadata

If `Propose(ACQUIRE)` fails, `remove_pending` cleans up the local pending slot and a compensating `RELEASE` is appended via `AppendLocalEntry`. If `Propose(RELEASE)` fails, `ScopeExit` retries the release on cleanup.

Important detail:

- `lock_wait_ms`, `blocking_agent_id`, `blocking_similarity_score`, `wait_position`, `wake_count`, `queue_hops`, and `active_lock_count` are all exposed in the `AcquireResponse`

---

## 6. Lock Table and Queue Model

### 6.1 Current active-lock structure

The lock table currently uses:

- `unordered_map<string, SemanticLock> active_`

Each active lock stores:

- `agent_id`
- `centroid`
- `threshold`
- `waiters`

Each waiter stores:

- waiting agent id
- embedding
- theta
- per-waiter condition variable
- ready/granted flags
- queue hop count

### 6.2 Admission rule

For every incoming request:

- compare the incoming embedding against all active lock centroids
- compute cosine similarity
- if any similarity is `>= theta`, the request conflicts
- choose the highest-similarity conflicting active lock as the current blocker

### 6.3 Queue behavior

The queue model is not one global queue.

Instead:

- each active lock owns its own waiter deque
- a blocked request queues behind the active lock with the strongest current conflict
- when a lock releases, its queued waiters are reconsidered from the front
- each waiter is rechecked against the current active set
- if it still conflicts, it is moved to another active lock's queue
- that move increments `queue_hops`

### 6.4 What queue hops mean

`queue_hops` is not a network metric.
It means:

- how many times a waiting request had to be requeued behind another active lock before it was finally granted

Interpretation:

- `0` hops: waited once behind the original blocker and then ran
- `1+` hops: original blocker released, but the waiter still conflicted with someone else and got moved again
- high hop counts indicate queue churn and potential fairness pressure

### 6.5 Current live queue telemetry

The lock table now emits live log lines for:

- `[LOCK_QUEUE]`
- `[LOCK_REQUEUE]`
- `[LOCK_GRANT]`

These lines include:

- agent id
- blocking agent
- similarity that caused queueing or requeueing
- queue position
- queue hops
- theta

This is the current way to observe "why did this request enter a queue?" in real time.

---

## 7. Distributed Coordination Model

### 7.1 Raft role in the current system

Raft currently handles:

- leader election
- append-based replication of lock transitions
- commit/apply sequencing
- leader discovery for the proxy

The replicated log entry types are:

- `ACQUIRE`
- `RELEASE`

### 7.2 What the apply callback does

Each node wires the Raft apply callback into the lock table:

- `ACQUIRE` -> `lock_table.apply_acquire(...)`
- `RELEASE` -> `lock_table.apply_release(...)`

That means followers learn lock state by replaying committed log entries.

### 7.3 Current coordination limitation

Local leader admission happens before durable cluster agreement on `ACQUIRE`.

So the current design has this property:

- the leader may hold a semantic lock locally before the acquire is fully committed

That is a meaningful architectural caveat and should be considered the first thing to revisit if correctness behavior becomes the main priority.

---

## 8. Current Proto and Service Surface

### 8.1 Client-facing service

`proto/dscc.proto` defines:

- `Ping`
- `AcquireGuard`
- `ReleaseGuard`

Important `AcquireResponse` fields currently used by the benchmark:

- `server_received_unix_ms`
- `lock_acquired_unix_ms`
- `qdrant_write_complete_unix_ms`
- `lock_released_unix_ms`
- `lock_wait_ms`
- `blocking_similarity_score`
- `blocking_agent_id`
- `leader_redirect`
- `serving_node_id`
- `wait_position`
- `wake_count`
- `queue_hops`
- `active_lock_count`

### 8.2 Raft service

`proto/dscc_raft.proto` defines:

- `RequestVote`
- `AppendEntries`
- `InstallSnapshot`
- `GetLeader`

The current public leader-discovery path used by the proxy is `GetLeader`.

---

## 9. Inputs and Workload Model

Primary semantic inputs currently live in:

- `demo_inputs/A.json`
- `demo_inputs/B.json`
- `demo_inputs/C.json`
- `demo_inputs/D.json`
- `demo_inputs/E.json`

These JSON files currently contain:

- a top-level `payload`
- top-level scheduling/operation metadata
- a `payload_schedule` array for multiple payload variants

Current benchmark behavior:

- the benchmark runner loads all unique payload texts from these JSON files
- requests embeddings once for each unique payload
- uses those embeddings as the benchmark template corpus

Role mapping currently used by the curated benchmark runner:

- `A.json` -> `sustainability_agent_*`
- `B.json` -> `safety_agent_*`
- `C.json` -> `cost_agent_*`
- `D.json` -> `construction_agent_*`
- `E.json` -> `client_agent_*`

---

## 10. Testing and Evaluation Infrastructure

### 10.1 `src/testbench.cpp`

Purpose:

- in-process lock-table testing
- fairness and queue behavior testing
- no Docker, no proxy, no Qdrant

### 10.2 `src/e2e_bench.cpp`

Purpose:

- end-to-end system demo
- embedding generation
- gRPC calls through the full system
- Qdrant validation
- readable scenario timelines

### 10.3 `src/benchmark_runner.cpp`

Purpose:

- curated benchmarking against the full distributed stack
- role-based agent naming
- live queue-event streaming
- per-case timeline output
- per-case results summary
- raw JSON log generation
- matrix and soak experiment modes

Current scenario set (13 cases):

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
11. The Paraphrase Gauntlet — correctness under paraphrase: does the model detect near-duplicate phrasing?
12. The Cross-Domain Flood — specificity: does the model incorrectly block unrelated domains?
13. The Write Pressure Ratchet — fairness under sustained write load: are readers consistently served?

Cases 11–13 are research-purpose scenarios designed to compare embedding model behaviour.
Results and metric definitions are in `MODEL_FINDINGS.md`.

### 10.4 Current benchmark outputs

**Single mode** emits two outputs:

1. terminal output — live queue events, final timeline per case, final results per case

2. raw JSON — `logs/benchmark_run_<timestamp>.json`
   - case metadata, metrics, container stats, per-operation traces, correctness-violation records

**Matrix mode** emits:

- one JSON per (model, theta) combination — `logs/benchmark_run_<ts>_<profile>_<theta>.json`
- one shared CSV — `logs/benchmark_run_<ts>_matrix.csv`
  - one row per (profile × theta × scenario)
  - columns: profile, model_id, theta, scenario, total_ops, write_ops, read_ops,
    throughput_ops_per_sec, utilization_factor, latency_p50/p95/p99_ms,
    write/read_latency_p95_ms, lock_wait_p50/p95/p99_ms, qdrant_window_p95_ms,
    blocked_rate, blocked_ops, conflicting_overlap_violations, serialization_score,
    distinct_parallelism_rate, embedding_p50/p95/p99_ms

**Soak mode** emits:

- one time-series CSV — `logs/soak_run_<timestamp>.csv`
  - one row per snapshot interval (default every 60 s)
  - columns: elapsed_sec, window_ops, total_ops, qdrant_size, lock_wait_p50/p95/p99_ms,
    op_latency_p50/p95/p99_ms, qdrant_window_p95_ms, blocked_rate, throughput_ops_per_sec

---

## 11. Current Correctness Metric

### 11.1 What "correctness" means in the benchmark today

For every pair of operations in a scenario:

- compute pairwise cosine similarity
- if similarity is `>= theta`, treat the pair as a conflict pair
- compare their lock-hold intervals:
  - `[lock_acquired_unix_ms, lock_released_unix_ms)`
- if two conflicting operations overlap, count a correctness violation

So "correctness" currently means:

- semantic conflict pairs should not overlap in the active critical section

### 11.2 Current timeline behavior for violations

The benchmark timeline now inserts explicit red violation events.

These events are emitted:

- when the second conflicting lock in an overlapping pair became active

So the timeline can now show:

- lock granted
- then violation detected at that grant point

### 11.3 What a violation means

A correctness violation means:

- two operations whose pairwise similarity was at or above `theta`
- were both considered active at the same time according to the recorded timestamps

This may indicate:

- a real semantic overlap bug
- or a lifecycle/timestamp-boundary problem in how active intervals are defined

Right now the benchmark should be treated as a strong signal, not an absolute proof of root cause.

---

## 12. Current Observed Benchmark State

### 12.1 Single-mode correctness

Some scenarios achieve clean behaviour across all runs:
- `The Strict Sieve`, `The Ghost Client`, `The Almost Collision`, `The Mixed Stagger`

Some scenarios still report occasional correctness violations:
- `The Thundering Herd`, `The Semantic Interleaving`, `The Read-Starvation Trap`,
  `The Permissive Sieve`, `Queue Hopping`, `The 100% Read Stampede`

The lock table and benchmark infrastructure are active and meaningful. The correctness
story for the existing 10 scenarios is not yet fully settled.

### 12.2 Embedding model evaluation (matrix mode)

A full 3 × 3 × 13 matrix sweep was completed comparing `all-minilm:latest` (ollama),
`bge-m3:latest` (bge), and `qwen3-embedding:0.6b` (qwen) at θ ∈ {0.55, 0.75, 0.95}.

Key findings (full metric definitions and tables in `MODEL_FINDINGS.md`):

- **`all-minilm` structurally fails paraphrase detection.** On the Paraphrase Gauntlet
  its serialisation score never exceeds 0.833 at any theta. At θ=0.75 it allows 6 out of
  12 conflict pairs to run concurrently. No threshold tuning fixes this — the model
  lacks the semantic resolution to distinguish paraphrases from distinct payloads.

- **`qwen3-embedding:0.6b` at θ=0.75 is the recommended operating point.** It is the
  only (model, θ) combination that achieves a perfect serialisation score (1.000) on the
  Paraphrase Gauntlet with zero violations, while also avoiding false-positive cross-domain
  blocking (Case 12 converges to the same latency as `all-minilm` at θ=0.75).

- **Reader–writer fairness is model-agnostic.** Case 13 (Write Pressure Ratchet) shows
  write and read P95 latencies within 60 ms of each other across all nine (model, θ)
  combinations. The DSLM lock table enforces this at the architecture level.

- **Embedding overhead:** `all-minilm` p50 ≈ 10 ms; `bge`/`qwen` p50 ≈ 111–133 ms.
  The ~11× difference is a fixed per-operation cost; it does not scale with concurrency.

### 12.3 Soak mode

The soak test is implemented and ready to run (`DSLM_RUN_MODE=soak`). It has not yet
been run for a full 2-hour session. The experiment will validate whether lock-wait
latency stays flat as the Qdrant collection grows — the expected result is that
serialisation overhead (in-memory) is DB-size-independent, while Qdrant upsert window
may grow marginally at high collection sizes.

---

## 13. Files That Matter Most

If you are changing system behavior, these are the most important files:

- `src/active_lock_table.h`
- `src/active_lock_table.cpp`
- `src/lock_service_impl.h`
- `src/lock_service_impl.cpp`
- `src/raft_node.h`
- `src/raft_node.cpp`
- `src/proxy_service_impl.h`
- `src/proxy_service_impl.cpp`
- `src/benchmark_runner.cpp`
- `src/e2e_bench.cpp`
- `proto/dscc.proto`
- `proto/dscc_raft.proto`
- `docker-compose.yml`
- `README.md`

---

## 14. Key Documents

| Document | Purpose |
|---|---|
| `README.md` | Build commands, run commands for all three benchmark modes, plotting instructions |
| `STATE.md` | This file — authoritative current-state description |
| `expl_docs/VERSION_0.md` | Historical deep-dive architecture reference (formerly `CURRENT_SYSTEM.md`; note the caveat at the top — use `STATE.md` for current truth) |
| `expl_docs/VERSION_1.md` | Original architecture plan for the distributed system (designed for 3 nodes with a Go proxy; actual implementation uses 5 nodes with a C++ proxy — see header note in that file) |
| `MODEL_FINDINGS.md` | Embedding model evaluation results from the matrix sweep — metric definitions, per-case tables, recommendation |

---

## 15. Current Bottom Line

The repository is currently in this state:

- distributed runtime exists (Raft, proxy, 5-node cluster)
- semantic queueing exists (per-lock waiter queues, requeue, hop tracking)
- read and write paths both exist
- three benchmark modes exist: single, matrix, soak
- embedding model evaluation is complete: `qwen3-embedding:0.6b` at θ=0.75 recommended
- live queue visibility and explicit violation markers exist

But:

- benchmark correctness is not yet clean across all 10 original stress scenarios
- active lock state is still in-memory and non-durable
- there is no lease-based expiry for abandoned locks
- soak test (2-hour DB growth experiment) has not yet been run to completion

So the repo should be treated as:

- a functioning distributed research prototype with a completed embedding model study
- not yet a fully closed or production-ready distributed lock system
