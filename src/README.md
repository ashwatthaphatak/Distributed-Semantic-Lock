# src — C++ Source Modules

**Authors: Ashwattha Phatak, Ayush Gala**  
CSC 724 — Advanced Distributed Systems, NC State University

---

## Module index

### Lock service (Ashwattha Phatak)

| File | Purpose |
|---|---|
| `active_lock_table.h / .cpp` | In-memory semantic admission table. Maintains the set of active `SemanticLock` entries and their waiter queues. On `acquire`, computes cosine similarity against every active lock and either admits the request or enqueues it behind the conflicting holder. On `release`, pops the next waiter whose conflict has cleared and notifies it. Implements the waiter-rebalancing algorithm so a waiter always tracks its current blocking lock. |
| `lock_service_impl.h / .cpp` | gRPC `LockService` implementation. `AcquireGuard` is the critical path: it calls `wait_for_admission`, proposes an ACQUIRE entry to Raft, waits for the entry to be applied, performs the Qdrant operation, proposes a RELEASE, and returns. Includes ghost-lock prevention via a compensating RELEASE on Propose timeout. |
| `e2e_bench.cpp` | End-to-end integration harness. Starts multiple concurrent agent threads, each firing a sequence of acquire/release RPCs against the proxy, and validates that no two conflicting lock intervals overlap. |
| `e2e_demo.cpp` | Interactive single-run demo. Sends the `demo_inputs/` agent profiles through the live stack and prints a human-readable timeline. |
| `paraphrase_gauntlet_demo.cpp` | Cross-model paraphrase detection study. Sends 12 paraphrase pairs through each embedding model at three θ values and records whether the semantic lock correctly serializes them. |

### Raft consensus (Ayush Gala)

| File | Purpose |
|---|---|
| `raft_node.h / .cpp` | Core Raft engine. Manages election timers, heartbeat loops, and log replication. Implements `RequestVote`, `AppendEntries`, and `InstallSnapshot` state-machine transitions. Exposes `Propose` (append a command to the log) and `WaitUntilApplied` (block until a given index is committed and applied). |
| `raft_service_impl.h / .cpp` | gRPC `RaftService` — thin shim that unmarshals incoming Raft RPCs and delegates to `RaftNode`. |
| `raft_test.cpp` | 14 in-process Raft regression scenarios: happy path, leader crash, follower catch-up, split vote, semantic conflict under replication. No Docker required. |

### Proxy (Ayush Gala)

| File | Purpose |
|---|---|
| `proxy_service_impl.h / .cpp` | Leader-aware forwarding proxy. Runs a background `LeaderPollLoop` that queries each node for the current leader and caches the result. `AcquireGuard` and `ReleaseGuard` use `ForwardWithLeaderRetry` — on a `NOT_LEADER` status, it refreshes leader state and retries transparently. Caches gRPC channels by address to avoid per-request channel creation. |
| `proxy_main.cpp` | Proxy binary entrypoint. Reads `PROXY_PORT` and `BACKEND_NODES` from the environment, constructs a `ProxyServiceImpl`, and starts the gRPC server. |

### Node lifecycle (Ayush Gala)

| File | Purpose |
|---|---|
| `main.cpp` | DSCC node entrypoint. Wires together `RaftNode`, `LockServiceImpl`, and `RaftServiceImpl` onto a single gRPC server with two ports (LockService + RaftService). Handles `SIGTERM`/`SIGINT` for graceful shutdown. |
| `threadsafe_log.h / .cpp` | Lock-protected `std::ostream` wrapper. `log_line` serializes multi-field log entries without interleaving output from concurrent threads. |

### Benchmark and test harnesses (Ayush Gala)

| File | Purpose |
|---|---|
| `benchmark_runner.cpp` | Curated 13-scenario benchmark runner. Supports single mode (one run), matrix mode (3 models × 3 thresholds × 13 scenarios), and soak mode (continuous load for configurable duration). Writes JSON and CSV result files to `logs/`. |
| `testbench.cpp` | 28 in-process `ActiveLockTable` unit tests. Covers FIFO ordering, parallel reads, conflict serialization, and waiter rebalancing. No Docker required. |
