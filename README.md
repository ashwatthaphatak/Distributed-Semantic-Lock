# DSCC — Distributed Semantic Concurrency Control

**Authors: Ashwattha Phatak, Ayush Gala**  
CSC 724 — Advanced Distributed Systems, NC State University

---

## What this is

DSCC is a distributed, Raft-replicated semantic lock manager that sits in front of a Qdrant vector database.  
Instead of locking on key equality, it locks on *meaning*: two requests conflict when the cosine similarity of their embedding vectors is ≥ a configurable threshold θ.

This lets collaborating AI agents—or any producer/consumer system that works with natural-language data—coordinate access without knowing each other's exact identifiers.

---

## Repository layout

```
.
├── src/                    C++ source — lock service, Raft, proxy, benchmarks
│   ├── active_lock_table.{h,cpp}     Semantic admission table (Ashwattha Phatak)
│   ├── lock_service_impl.{h,cpp}     gRPC lock service (Ashwattha Phatak)
│   ├── raft_node.{h,cpp}             Raft consensus engine (Ayush Gala)
│   ├── raft_service_impl.{h,cpp}     Raft gRPC service (Ayush Gala)
│   ├── proxy_service_impl.{h,cpp}    Leader-aware proxy (Ayush Gala)
│   ├── proxy_main.cpp                Proxy entrypoint (Ayush Gala)
│   ├── main.cpp                      Node entrypoint (Ayush Gala)
│   ├── threadsafe_log.{h,cpp}        Thread-safe logging (Ayush Gala)
│   ├── e2e_bench.cpp                 End-to-end integration harness (Ashwattha Phatak)
│   ├── e2e_demo.cpp                  Interactive demo harness (Ashwattha Phatak)
│   ├── benchmark_runner.cpp          Curated 13-scenario benchmark (Ayush Gala)
│   ├── testbench.cpp                 ActiveLockTable unit tests (Ayush Gala)
│   ├── raft_test.cpp                 Raft regression tests (Ayush Gala)
│   └── paraphrase_gauntlet_demo.cpp  Cross-model paraphrase study (Ashwattha Phatak)
│
├── proto/                  Protobuf / gRPC definitions (Ashwattha Phatak)
│   ├── dscc.proto          Client-facing LockService API
│   └── dscc_raft.proto     Internal Raft RPC messages
│
├── scripts/
│   ├── workload/                     Workload generators — produce logs
│   │   ├── locustfile.py             Full DSLM gRPC load generator (Ayush Gala)
│   │   ├── locustfile_base.py        Qdrant-direct baseline load generator (Ayush Gala)
│   │   ├── thundering_herd.py        Standalone burst generator (Ayush Gala)
│   │   ├── agent_request.sh          Single-agent request helper (Ayush Gala)
│   │   └── compare_baseline.sh       Runs both workloads + overhead plot (Ayush Gala)
│   ├── analysis/                     Log processors — read logs/, write plots/
│   │   ├── plot_matrix_metrics.py    Model comparison plots (Ashwattha Phatak)
│   │   ├── plot_model_comparison.py  Timing comparison across models (Ashwattha Phatak)
│   │   ├── plot_overhead.py          Lock-overhead analysis (Ashwattha Phatak)
│   │   ├── plot_benchmark_report.py  Single-run timing report (Ayush Gala)
│   │   └── plot_soak_test.py         Soak-test latency plots (Ayush Gala)
│   └── plots/                        Generated PNG output (gitignored)
│
├── demo_inputs/            JSON agent profiles for demo and benchmark runs
├── logs/                   Generated benchmark output (gitignored)
├── docker/                 Per-service Dockerfiles
├── docker-compose.yml      Full local stack definition
├── CMakeLists.txt          Build system
├── SETUP.md                Prerequisites, build, run, and test instructions
├── DEMO.md                 Step-by-step demo walkthrough
└── MODEL_FINDINGS.md       Experimental results — embedding model comparison
```

---

## Architecture overview

```
Client request (natural-language payload + embedding vector)
    │
    ▼
dscc-proxy  ─── leader poll loop ──▶  (discovers current Raft leader)
    │
    ▼ ForwardWithLeaderRetry
dscc-node (leader)
    │
    ├─▶ ActiveLockTable::wait_for_admission()
    │       cosine_similarity(incoming, each active lock) ≥ θ  →  enqueue
    │       similarity < θ for all active locks              →  admit
    │
    ├─▶ Raft::Propose(ACQUIRE)   ──▶  replicated to 4 followers
    │
    ├─▶ Qdrant read / write
    │
    └─▶ Raft::Propose(RELEASE)  ──▶  replicated to 4 followers
            │
            ▼
        ActiveLockTable::apply_release()  →  wake next waiter
```

Five DSCC nodes form a Raft cluster. Each node runs both a `LockService` (client-facing) and a `RaftService` (internal replication). The leader-aware proxy discovers the current leader and retries on redirect so clients never need to track leadership themselves.

---

## Quick start

See [SETUP.md](SETUP.md) for full prerequisites and build instructions.

```bash
# 1. Build the binaries
cmake -S . -B /tmp/dslm_build
cmake --build /tmp/dslm_build --target dscc-node dscc-proxy dscc-e2e-bench dscc-benchmark -j"$(nproc)"

# 2. Start the Docker stack
docker compose up -d --build \
    qdrant embedding-service \
    dscc-node-1 dscc-node-2 dscc-node-3 dscc-node-4 dscc-node-5 \
    dscc-proxy

# 3. Run the end-to-end demo
/tmp/dslm_build/dscc-e2e-bench
```

---

## Running tests

```bash
# In-process unit tests (no Docker needed)
/tmp/dslm_build/dscc-testbench      # 28 ActiveLockTable scenarios
/tmp/dslm_build/dscc-raft-test      # 14 Raft regression scenarios

# Full integration suite (requires running stack)
/tmp/dslm_build/dscc-e2e-bench
```

---

## Key documents

| Document | Contents |
|---|---|
| [SETUP.md](SETUP.md) | Full build, run, and test reference |
| [DEMO.md](DEMO.md) | Step-by-step demo walkthrough |
| [MODEL_FINDINGS.md](MODEL_FINDINGS.md) | Experimental results across embedding models |
| [src/README.md](src/README.md) | Source module descriptions and authors |
| [scripts/README.md](scripts/README.md) | Plotting scripts and how to run them |
| [demo_inputs/README.md](demo_inputs/README.md) | Agent profiles and semantic overlap design |

---

## Authors

| Contributor | Modules |
|---|---|
| **Ashwattha Phatak** | ActiveLockTable, LockService, proto definitions, cosine similarity math, e2e harness, embedding model study (matrix/overhead/model-comparison plots, MODEL_FINDINGS.md) |
| **Ayush Gala** | Raft core (election, heartbeat, log replication), node lifecycle, proxy + leader discovery, benchmark harness, testbench, Locust workload generation, soak/report plots |
