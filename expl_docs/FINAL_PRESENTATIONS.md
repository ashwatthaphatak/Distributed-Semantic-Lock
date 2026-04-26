# DSCC / DSLM — Exhaustive Technical Reference

---

## 1. Project Overview & Purpose

### 1.1 Problem Statement

When multiple AI agents concurrently write to a shared vector memory store (Qdrant), semantically overlapping writes may corrupt or overwrite each other's contributions. Traditional locking mechanisms (row-level, key-based) cannot detect this because the conflict domain is *semantic*: two requests conflict not because they touch the same key or row, but because their natural-language payloads are semantically similar as measured by their embedding vectors.

### 1.2 Core Thesis

The system is built around the hypothesis that cosine similarity between embedding vectors can serve as a real-time conflict-detection mechanism for concurrent agent writes. If `cosine_similarity(embedding_A, embedding_B) >= theta`, the two operations are treated as semantically conflicting and must be serialized. If the similarity is below theta, the operations may proceed in parallel. The threshold theta is a tunable parameter that governs the sensitivity boundary between conflict and independence.

### 1.3 What the System Achieves (Evidenced)

- A distributed semantic lock manager running as a five-node Raft cluster with a leader-aware proxy.
- In-memory semantic lock table with per-lock waiter queues, queue-hop tracking, and FIFO-ordered rebalancing.
- Both write and read operations go through the same semantic admission path; reads are blocked only by active writes that share semantic similarity above theta.
- Qdrant integration for vector persistence (writes) and vector search (reads) after lock acquisition.
- A curated benchmark suite of 13 scenarios with three execution modes (single, matrix, soak).
- A completed embedding model evaluation across three models (`all-minilm:latest`, `bge-m3:latest`, `qwen3-embedding:0.6b`) at three thresholds (0.55, 0.75, 0.95), totaling 117 case runs.
- The recommendation, backed by experimental evidence, that `qwen3-embedding:0.6b` at theta=0.75 is the only (model, theta) combination achieving perfect paraphrase serialization (score 1.000) with zero violations.

### 1.4 Explicit Scope Boundaries (What the System Does NOT Do)

Per `TODO.md` and `STATE.md`:

- No durable snapshot/restore path for active lock state. Lock state is purely in-memory.
- No lease-based expiration or heartbeat-based cleanup for abandoned locks.
- No lock ownership enforcement on release (any caller can release any lock).
- No bounded waiting or timeout support for blocked acquisitions.
- No embedding dimension validation across requests before lock acquisition.
- No CI/CD pipeline.
- No persistent recovery story for active lock state after process restart.
- No pre-vote mechanism for Raft (acknowledged in `DEMO.md` as a known gap referenced by Diego Ongaro's thesis).

---

## 2. System Architecture

### 2.1 High-Level Architecture

The end-to-end request path, as documented in `README.md`:

```text
demo_inputs/*.json
    -> embedding-service (Ollama)
    -> dscc-proxy (leader-aware gRPC forwarder)
    -> current dscc-node leader
    -> ActiveLockTable (semantic admission)
    -> Raft ACQUIRE replication
    -> Qdrant read/write
    -> Raft RELEASE replication
```

### 2.2 Subsystem Decomposition

**embedding-service**: An Ollama container serving embedding generation via the `/v1/embeddings` HTTP API. Supports multiple models simultaneously (all-minilm, bge-m3, qwen3-embedding) selected per-request via the `model` field. Exposed on host port 7997, mapping to container port 11434. Model weights are cached in `.cache/ollama` via a Docker volume mount.

**dscc-proxy**: A C++ gRPC service (`src/proxy_main.cpp`, `src/proxy_service_impl.cpp/.h`). Originally planned as a Go proxy (per `expl_docs/VERSION_1.md`); the decision was made to keep the entire stack in C++ for build simplicity and a single container image. The proxy polls the cluster for leader information via `RaftService::GetLeader`, forwards `AcquireGuard` and `ReleaseGuard` to the current leader, retries on `NOT_LEADER`, `UNAVAILABLE`, and `DEADLINE_EXCEEDED`, and caches gRPC channels per backend address.

**dscc-node** (5 instances): Each node runs two gRPC services on separate ports — the client-facing `LockService` (port 50051) and the inter-node `RaftService` (port 50061). Each node owns an `ActiveLockTable`, a `RaftNode`, a `LockServiceImpl`, and a `RaftServiceImpl`.

**Qdrant**: Vector database for point persistence (writes via `PUT /collections/{col}/points?wait=true`) and vector search (reads via `POST /collections/{col}/points/search`). Uses cosine distance. Collection creation is idempotent (handles 200, 201, 409, and 400-already-exists responses).

### 2.3 Synchronous vs. Asynchronous Boundaries

- **Synchronous**: The `AcquireGuard` RPC is fully synchronous from the client's perspective. The proxy blocks until the leader responds. The leader blocks on `wait_for_admission` (condition variable wait), then blocks on `Raft::Propose` + `WaitUntilApplied` for both ACQUIRE and RELEASE.
- **Asynchronous**: Raft heartbeats run on a dedicated thread (`HeartbeatLoop`, sleeping `config_.heartbeat_ms` between iterations). Election timer runs on its own thread (`ElectionTimerLoop`, polling every 10ms). The apply loop (`ApplyLoop`) processes committed entries asynchronously from condition variable signals. Leader change callbacks fire on detached threads.

### 2.4 Stateful vs. Stateless Components

- **Stateful**: `dscc-node` — owns the in-memory `ActiveLockTable` and the Raft log (both purely in-memory, no persistence). Qdrant — persists vector points to disk.
- **Stateless**: `dscc-proxy` — maintains only a cached leader address and gRPC channel pool. `embedding-service` — stateless inference endpoint (model weights are read-only after loading).

### 2.5 CAP Tradeoff Positioning

The system prioritizes **Consistency** over **Availability**. Raft requires a quorum (3 out of 5 nodes) to commit any log entry. If quorum is lost, all writes block and eventually fail. This is explicitly tested in the e2e demo's "Quorum Collapse" phase (file `src/e2e_demo.cpp`, line 897–942) and in the benchmark runner's multi-node scenarios. Partition tolerance is achieved via Raft's standard mechanisms: isolated nodes cannot commit, and followers catch up via `AppendEntries` on rejoin.

### 2.6 Deployment Topology

Defined in `docker-compose.yml`:

- 1 Qdrant container (port 6333)
- 1 Ollama embedding-service container (host:7997 → container:11434)
- 5 dscc-node containers (ports 50051–50055 for service, 50061–50065 for Raft)
- 1 dscc-proxy container (port 50050)

A second compose file `docker-compose.server.yml` provides the same topology minus the embedding-service, for split client/server demos over Tailscale.

---

## 3. Components & Modules — Deep Dive

### 3.1 `ActiveLockTable` (`src/active_lock_table.h`, `src/active_lock_table.cpp`)

**Responsibility**: The core semantic conflict-detection and admission-control layer. Decides whether an incoming request must wait based on cosine similarity of its embedding against all active lock centroids.

**Public Interface**:

- `AcquireTrace acquire(agent_id, embedding, threshold)` — Legacy blocking acquire used by the testbench. Loops: find conflict → enqueue behind strongest-matching active lock → wait on per-waiter CV → wake → re-check or accept handoff.
- `AcquireTrace wait_for_admission(agent_id, embedding, threshold)` — Production blocking acquire used by `LockServiceImpl`. Same blocking loop as `acquire` but inserts a **pending** slot instead of a real lock. Blocks during `sweep_in_progress_` to prevent races with leader-change sweeps.
- `void remove_pending(agent_id)` — Removes a pending slot that was never promoted (e.g., Raft Propose failed). Rebalances any waiters queued behind it.
- `void release(agent_id)` — Removes the lock, rebalances orphaned waiters.
- `void apply_acquire(agent_id, embedding, threshold)` — Raft apply callback. Promotes a pending slot to a real lock, or inserts a new real lock if none exists.
- `void apply_release(agent_id)` — Raft apply callback. Delegates to `release()`.
- `size_t size() / active_count()` — Returns count of entries in `active_` map.
- `vector<string> active_agent_ids()` — Sorted snapshot of current lock holders.
- `vector<string> begin_leader_sweep()` / `void end_leader_sweep()` — Atomically gates new admissions and returns all held agent IDs for orphan cleanup after a leader change.
- `static float cosine_similarity(a, b)` — Computes cosine similarity in double precision, clamped to [-1, 1].

**Internal Data Structures**:

- `unordered_map<string, SemanticLock> active_` — Active locks keyed by agent ID.
- `mutex mu_` — Single mutex protecting all mutable state.
- `bool sweep_in_progress_` / `condition_variable sweep_cv_` — Gate for leader sweep.

**Key Algorithms**:

- `find_conflict_locked(embedding, threshold)` — Linear scan over all active locks. Computes cosine similarity for each. Returns the lock with the highest similarity that meets or exceeds the threshold. O(n) where n is the number of active locks.
- `rebalance_waiters_locked(waiters, granted_waiters)` — When a lock releases, its waiter deque is processed front-to-back. Each waiter is re-checked against the current active set. If it still conflicts, it's moved to the new strongest-matching lock's queue (incrementing `queue_hops`). If no conflict remains, it gets a pending slot and is granted via CV notification.
- `cosine_similarity` — Uses `double` accumulation for dot product, norm_a, norm_b. Handles empty/mismatched vectors by returning 0.0f. Clamps result to [-1, 1].

**Configuration**: Theta threshold is passed per-call, not stored globally. This enables per-case theta in the benchmark runner.

**Known Limitations**: No lock-ownership enforcement. No timeout on waits. Linear scan conflict detection is O(n) per admission check.

### 3.2 `LockServiceImpl` (`src/lock_service_impl.h`, `src/lock_service_impl.cpp`)

**Responsibility**: The gRPC service implementation bridging incoming `AcquireGuard`/`ReleaseGuard` RPCs to the lock table, Raft log, and Qdrant.

**Public Interface (gRPC methods)**:

- `Ping(PingRequest) -> PingResponse` — Returns "pong from {node_id} to {from_node}".
- `AcquireGuard(AcquireRequest) -> AcquireResponse` — The main entry point. Three-phase operation.
- `ReleaseGuard(ReleaseRequest) -> ReleaseResponse` — Explicit release via Raft.

**AcquireGuard Three-Phase Flow**:

1. **Phase 1 — Admission**: Validates `agent_id` and `embedding` are non-empty. Checks `raft_->IsLeader()`; if not leader, returns `FAILED_PRECONDITION` with `leader_redirect` metadata. Calls `lock_table_->wait_for_admission()` to block until admitted with a pending slot.

2. **Phase 2 — Raft ACQUIRE**: Constructs a `LogEntry` with `ACQUIRE` op type, proposes via `raft_->Propose()` with `raft_propose_timeout_ms_` timeout. Waits for apply via `raft_->WaitUntilApplied()`. If Propose fails: calls `remove_pending()` on the lock table, appends a compensating `RELEASE` via `AppendLocalEntry()` to cancel any ghost ACQUIRE on replicas, returns `UNAVAILABLE`.

3. **Phase 3 — Qdrant + Release**: For writes: calls `upsert_embedding_to_qdrant()`. For reads: calls `query_embedding_from_qdrant()`. If `lock_hold_ms_ > 0` and operation is write, sleeps for that duration. Proposes `RELEASE` via Raft. Uses a `ScopeExit` guard to ensure release is attempted even on error paths.

**Qdrant HTTP Client**: Raw POSIX socket implementation using `getaddrinfo`, `connect`, `send`, `recv`. No external HTTP library. Constructs HTTP/1.1 requests manually with `Connection: close`. Parses response status code from the first line.

- `upsert_embedding_to_qdrant`: Retries up to 3 times with backoff of `75ms * attempt`. Accepts 200 or 201 as success. Retries on 500 with "Please retry" in body.
- `query_embedding_from_qdrant`: Single attempt, `POST /collections/{col}/points/search` with `limit:3, with_payload:false, with_vector:false`.
- `ensure_qdrant_collection`: Idempotent PUT. Accepts 200, 201, 409, or 400-with-"already exists".

**Point ID Generation**: `make_numeric_point_id` uses FNV-1a hash (64-bit) of the agent_id string, then mixes with `timestamp_unix_ms << 22` XOR'd with the low 22 bits of the hash. Clamped to positive int64.

### 3.3 `RaftNode` (`src/raft_node.h`, `src/raft_node.cpp`)

**Responsibility**: In-memory Raft consensus node. Handles leader election, log replication, and commit/apply sequencing.

**Public Interface**:

- `Propose(entry, timeout, *committed_index)` — Leader-only. Appends entry to local log, then enters a retry loop: spawns one thread per peer for parallel `ReplicateToFollower` calls, checks if `commit_index_ >= proposed_index`, waits on `commit_cv_` with 25ms poll intervals until deadline.
- `AppendLocalEntry(entry)` — Appends without waiting for quorum. Used for compensating RELEASE entries.
- `WaitUntilApplied(index, timeout)` — Blocks on `apply_cv_` until `last_applied_ >= index`.
- `HandleRequestVote / HandleAppendEntries / HandleInstallSnapshot / HandleGetLeader` — Raft RPC handlers.
- `SetLeaderChangeCallback(callback)` — Registers callback for orphan sweep on leader transitions.
- Accessors: `IsLeader()`, `State()`, `LeaderAddress()`, `NodeId()`, `ServiceAddress()`, `CurrentTerm()`, `CommitIndex()`, `LastApplied()`, `LogSize()`, `Running()`.

**Internal Threading Model**:

Three dedicated threads:
1. `ElectionTimerLoop` — Polls every 10ms. If not LEADER and `steady_clock::now() >= election_deadline_`, triggers `StartElection()`.
2. `HeartbeatLoop` — If LEADER, spawns one thread per peer for `ReplicateToFollower()`, then sleeps `config_.heartbeat_ms` (default 75ms).
3. `ApplyLoop` — Waits on `apply_cv_` for `last_applied_ < commit_index_`. Increments `last_applied_`, reads the log entry, calls `on_commit_()` callback, then notifies `apply_cv_` again.

**Election Mechanism**: Standard Raft election. `StartElection()` transitions to CANDIDATE, increments term, votes for self, resets election deadline, then sends `RequestVote` RPCs serially to all peers. Collects votes; if `votes >= QuorumSize()`, calls `BecomeLeaderLocked()`. QuorumSize = `(peer_count + 1) / 2 + 1`.

**Log Replication**: `ReplicateToFollower` sends `AppendEntries` with all entries from `next_index_[peer]` onward. On success, updates `match_index_` and `next_index_`, then calls `AdvanceCommitIndexLocked()`. On conflict (success=false), backs up `next_index_` using `conflict_index` from the response (fast log backup optimization).

**Commit Index Advancement**: `AdvanceCommitIndexLocked()` iterates from `log_.size() - 1` down to `commit_index_ + 1`. For each index, counts how many peers have `match_index >= index`. If `replicated >= QuorumSize()` and the entry's term equals `current_term_`, sets `commit_index_` and notifies waiters. Only commits entries from the current term (Raft safety property).

**Random Election Timeout**: Uses `thread_local std::mt19937` seeded from `std::random_device`. Uniform distribution over `[election_timeout_min_ms, election_timeout_max_ms]`.

**Single-Node Optimization**: If `peer_addresses_` is empty, the node immediately becomes leader on `Start()` and commits entries instantly (no quorum needed).

**Log Structure**: The log is a `vector<LogEntry>` with a sentinel entry at index 0 (term 0). Actual entries start at index 1. `LogSize()` returns `log_.size() - 1`.

### 3.4 `RaftServiceImpl` (`src/raft_service_impl.h`, `src/raft_service_impl.cpp`)

**Responsibility**: Thin gRPC wrapper delegating to `RaftNode`. Implements four RPCs: `RequestVote`, `AppendEntries`, `InstallSnapshot`, `GetLeader`. Each method is a one-liner that calls the corresponding `RaftNode::Handle*` method and returns `grpc::Status::OK`.

### 3.5 `ProxyServiceImpl` (`src/proxy_service_impl.h`, `src/proxy_service_impl.cpp`)

**Responsibility**: Leader-aware gRPC proxy that sits in front of the Raft cluster.

**Leader Discovery**: `LeaderPollLoop` runs on a dedicated thread, calling `RefreshLeader()` every `leader_poll_ms_` (default 100ms). `RefreshLeader()` iterates all backend nodes, calls `RaftService::GetLeader` on each with a `leader_rpc_timeout_ms_` deadline. Returns the address of whichever node reports itself as leader or reports a known leader.

**Request Forwarding**: `ForwardWithLeaderRetry` is a template method that handles up to 3 retry attempts. On each attempt: refreshes leader if needed, creates a `LockService::Stub` for the target, calls the RPC with `request_timeout_ms_` deadline. On `FAILED_PRECONDITION` / "NOT_LEADER": extracts leader redirect from trailing metadata, retries. On `UNAVAILABLE` or `DEADLINE_EXCEEDED`: refreshes leader, retries.

**Channel Caching**: Channels are stored in `unordered_map<string, shared_ptr<Channel>> channels_`. Channels are created lazily and reused, so leader flips do not recreate gRPC connections.

### 3.6 `main.cpp` (`src/main.cpp`)

**Responsibility**: Entry point for `dscc-node`. Reads all configuration from environment variables, constructs `ActiveLockTable`, `RaftNode`, `LockServiceImpl`, `RaftServiceImpl`, wires the Raft apply callback into the lock table, sets up the leader change callback for orphan sweep, starts gRPC server on two ports (service + raft).

**Leader Change Callback (Orphan Sweep)**: When a new leader is elected, the callback calls `lock_table.begin_leader_sweep()` to get all currently held agent IDs, then proposes `RELEASE` entries via Raft for each orphan. If leadership is lost during the sweep, it aborts. Calls `end_leader_sweep()` when done.

### 3.7 `proxy_main.cpp` (`src/proxy_main.cpp`)

**Responsibility**: Entry point for `dscc-proxy`. Reads configuration from environment variables (`PROXY_PORT`, `BACKEND_NODES`, `LEADER_POLL_MS`, `REQUEST_TIMEOUT_MS`, `LEADER_RPC_TIMEOUT_MS`). Creates `ProxyServiceImpl`, starts the background leader poll, starts gRPC server.

### 3.8 `threadsafe_log.h` / `threadsafe_log.cpp`

**Responsibility**: Thread-safe line logger. Uses a global `std::mutex` to serialize `std::cout` output. Single function: `void log_line(const string& line)`.

### 3.9 `benchmark_runner.cpp` (`src/benchmark_runner.cpp`)

**Responsibility**: Curated benchmark suite runner with three modes: single, matrix, soak. This is the largest source file (~3,060 lines).

**Scenario Definitions**: 13 curated scenarios defined as `BenchmarkCase` structs:

| # | Name | Agents | Writes | Reads | Theta | Lock Hold | Arrival |
|---|------|--------|--------|-------|-------|-----------|---------|
| 1 | The Thundering Herd | 10 | 10 | 0 | 0.55 | 750ms | Burst |
| 2 | The Semantic Interleaving | 10 | 10 | 0 | 0.55 | 750ms | Burst |
| 3 | The Read-Starvation Trap | 10 | 2 | 8 | 0.55 | 750ms | Stagger 40ms |
| 4 | The Permissive Sieve | 5 | 5 | 0 | 0.20 | 750ms | Burst |
| 5 | The Strict Sieve | 10 | 5 | 5 | 0.90 | 0ms | Burst |
| 6 | The Ghost Client | 5 | 5 | 0 | 0.55 | 750ms | Burst |
| 7 | The Almost Collision | 2 | 2 | 0 | 0.55 | 0ms | Burst |
| 8 | Queue Hopping | 20 | 20 | 0 | 0.55 | 0ms | Burst |
| 9 | The Mixed Stagger | 10 | 10 | 0 | 0.55 | 750ms | Stagger 100ms |
| 10 | The 100% Read Stampede | 10 | 0 | 10 | 0.55 | 0ms | Burst |
| 11 | The Paraphrase Gauntlet | 10 | 10 | 0 | 0.75 | 750ms | Burst |
| 12 | The Cross-Domain Flood | 12 | 12 | 0 | 0.75 | 500ms | Burst |
| 13 | The Write Pressure Ratchet | 16 | 4 | 12 | 0.75 | 800ms | Stagger 30ms |

**Scenario Descriptions** (verbatim from `build_curated_cases` in `benchmark_runner.cpp`):

- Case 1: "Verify strict serialization under massive semantic overlap."
- Case 2: "Verify two independent semantic hot spots can stay active in parallel."
- Case 3: "Verify read-heavy traffic still queues correctly behind conflicting writers."
- Case 4: "Verify a permissive theta collapses mixed semantics into a deep queue."
- Case 5: "Verify a strict theta preserves concurrency across mixed operations."
- Case 6: "Verify one long-held writer amplifies waiter depth without breaking ordering."
- Case 7: "Verify a near-threshold pair below theta stays concurrent."
- Case 8: "Verify bursty contention produces queue hops without losing progress."
- Case 9: "Verify mixed semantic pressure remains stable under long staggered arrivals."
- Case 10: "Verify fully read-only hot traffic still respects semantic exclusion."
- Case 11: "Test whether each model reliably detects paraphrase overlap; weak models risk violations."
- Case 12: "Verify distinct domains run in parallel; weak models cause unnecessary cross-domain blocking."
- Case 13: "Measure read fairness under sustained write load; consistent models produce predictable read latency."

**Matrix Mode**: Sweeps 3 embedding models × 3 thetas (0.55, 0.75, 0.95) × 13 scenarios = 117 case runs. Outputs a shared CSV plus per-combination JSON files.

**Soak Mode**: Continuous insertion for a configurable duration (default 2 hours). Takes windowed latency snapshots every `soak_snapshot_sec` (default 60s). Outputs a time-series CSV.

**Metrics Computed** (per `BenchmarkMetrics` struct): `total_ops`, `write_ops`, `read_ops`, `grpc_failures`, `granted_ops`, `blocked_ops`, `blocked_writes`, `blocked_reads`, `blocked_reads_on_write`, `expected_conflict_pairs`, `expected_distinct_pairs`, `conflicting_overlap_violations`, `distinct_parallel_pairs`, `distinct_nonparallel_pairs`, `active_lock_count_max`, `wait_position_max`, `wake_count_max`, `queue_hops_max`, `success_rate`, `throughput_ops_per_sec`, `utilization_factor`, `serialization_score`, `distinct_parallelism_rate`, `makespan_ms`, latency percentiles (p50/p95/p99), write/read latency p95, lock-wait percentiles, Qdrant window p95, queue position/wake count/queue hops p95.

### 3.10 `e2e_bench.cpp` (`src/e2e_bench.cpp`)

**Responsibility**: End-to-end demo harness. Starts the Docker Compose stack, waits for services, loads agent documents from `demo_inputs/`, generates embeddings, runs 7 scenarios (4 functional + leader failover + follower restart with quorum test + rejoined follower quorum test), validates Qdrant payloads, and prints timelines.

### 3.11 `e2e_demo.cpp` (`src/e2e_demo.cpp`)

**Responsibility**: Choreographed ~5-minute live demo driver. Runs continuous workload through 4 phases: steady state (t+0), leader failover (t+20s), node recovery (t+90s), quorum collapse (t+180s). Prints cluster health, workload stats, and colored ANSI output.

### 3.12 `testbench.cpp` (`src/testbench.cpp`)

**Responsibility**: In-process concurrency tests for `ActiveLockTable`. No Docker, no gRPC, no Qdrant. 5 scenarios:
1. Independent embeddings — verifies parallel execution (overlap expected).
2. Nearly identical embeddings — verifies serialization (no overlap).
3. FIFO within a hot conflict group — verifies queue ordering.
4. Unrelated work bypasses a hot region — verifies cold writes proceed while hot queue remains serialized.
5. Wakeups stay local — verifies releasing one hot lock does not wake waiters on another lock.

### 3.13 `raft_test.cpp` (`src/raft_test.cpp`)

**Responsibility**: In-process Raft regression testbench with real localhost gRPC, three `RaftNode` peers. 9 test suites:
- S1: Basic election + replication
- S2: Leader failover
- S3: Follower outage + cold restart catch-up (12 entries)
- S4: `AppendLocalEntry` eventual replication
- S5: ACQUIRE then RELEASE chain
- S6: Many entries (25) all peers up
- S7: Split-brain spot check (verifies at most one leader over 4 seconds)
- S8: Log truncation on conflicting entries (tests Raft safety: old leader's uncommitted entries overwritten by new leader)
- S9: Real `ActiveLockTable` wired through apply callback (tests `wait_for_admission` → `apply_acquire` → `apply_release` flow through Raft without deadlocks)

### 3.14 `paraphrase_gauntlet_demo.cpp` (`src/paraphrase_gauntlet_demo.cpp`)

**Responsibility**: Standalone demo that runs the Paraphrase Gauntlet and Cross-Domain Flood scenarios against all three embedding models at a fixed theta. Produces a confusion matrix (TP/FN/FP/TN) treating conflict detection as binary classification. Computes accuracy, precision, recall, F1, serialization score, distinct parallelism rate, and false positive rate per model. Outputs a structured JSON file.

### 3.15 Plotting Scripts (`scripts/`)

Five Python plotting scripts:

- `plot_benchmark_report.py` — Generates review-ready plots from a single-mode JSON run. Produces: latency bar chart, lock-wait bar chart, throughput/utilization charts, correctness violations chart. Groups scenarios into "hotspot", "mixed", "cold" categories with distinct colors.
- `plot_matrix_metrics.py` — Generates comparison bar charts from a matrix CSV. All plots use theta as the x-axis with grouped bars per model. Produces separate plots for "research cases" (11, 12, 13) covering serialization score, distinct parallelism rate, false positive rate, embedding latency.
- `plot_model_comparison.py` — Compares timing plots across embedding models. Groups benchmark JSON files by `model_id` and generates side-by-side comparisons.
- `plot_soak_test.py` — Plots time-series latency from a soak CSV. Produces lock-wait percentile over time, op latency over time, throughput over time, and blocked rate over time.
- `plot_overhead.py` — Compares Qdrant-direct baseline Locust run to full DSLM Locust run. Produces grouped bar charts (baseline vs DSLM p50) and delta bars (DSLM − baseline for p50/p95/p99). Always produces `overhead_summary.csv`; PNG only if matplotlib is available.

### 3.16 `agent_request.sh`

**Responsibility**: Standalone shell script for sending individual `AcquireGuard` requests from the command line. Requires `grpcurl`, `jq`, and `curl`. Converts plain-text payload into an embedding via Ollama `/v1/embeddings`, then sends a gRPC `AcquireGuard` request to the DSCC cluster. Supports both `read` and `write` operations. Parses the response for `granted` status and `leaderRedirect` for NOT_LEADER handling.

### 3.17 Demo Input Files (`demo_inputs/`)

13 JSON files (A.json through M.json), each representing an AI agent persona in an architectural design project:

| File | Agent Name | Role | Primary Operation |
|---|---|---|---|
| A.json | Ari | Sustainability-Focused Design Agent | write |
| B.json | Brooke | Safety and Code Compliance Agent | read |
| C.json | Casey | Cost and Budget Control Agent | read |
| D.json | Devon | Construction Logistics and Schedule Agent | write |
| E.json | Emerson | Client Experience and Program Quality Agent | read |
| F.json | Frankie | Structural Engineering Agent | write |
| G.json | Gray | MEP Coordination Agent | read |
| H.json | Harper | Facade Engineering Agent | read |
| I.json | Ira | BIM Coordination Agent | read |
| J.json | Jordan | Landscape and Urban Design Agent | write |
| K.json | Kai | Interior Architecture Agent | read |
| L.json | Lane | Geotechnical and Civil Engineering Agent | write |
| M.json | Morgan | Commissioning and Quality Assurance Agent | read |

Each file contains a `payload_schedule` array with 3–6 entries. Each entry has `scheduled_offset_ms`, `operation` (read/write), and `payload` (natural language text).

**Semantic Overlap Design**: Agent A's payloads are all paraphrases of the same concept ("Review the massing concept and prioritize passive cooling, daylight access, and low-carbon material options for the civic annex") — this is the primary test case for paraphrase detection. Agents A, B, and M share the identical top-level payload text. Agent D and E share payload text ("Assess construction phasing for the atrium steel package and map potential site-access bottlenecks"). Agents E and K share payload text ("Evaluate meeting-room mix against projected occupancy profiles and hybrid work patterns"). These overlaps are deliberate, designed to create known conflict pairs for correctness validation.

**Benchmark Agent Mapping** (from `role_prefix_for_source_file` in `benchmark_runner.cpp`):
- A.json → `sustainability_agent`
- B.json → `safety_agent`
- C.json → `cost_agent`
- D.json → `construction_agent`
- E.json → `client_agent`
- F–M → `agent` (generic prefix)

---

## 4. Data Structures

### 4.1 `SemanticLock` (active_lock_table.h)

```cpp
struct SemanticLock {
    std::string agent_id;
    std::vector<float> centroid;       // embedding vector
    float threshold;                    // theta at time of acquire
    bool pending = false;              // true before Raft commit promotes it
    std::deque<std::shared_ptr<WaitQueueEntry>> waiters;
};
```

### 4.2 `WaitQueueEntry` (active_lock_table.h)

```cpp
struct WaitQueueEntry {
    std::string waiting_agent_id;
    std::vector<float> embedding;
    float theta = 0.0f;
    std::shared_ptr<std::condition_variable> cv;  // per-waiter CV
    bool ready = false;    // "you may re-check now"
    bool granted = false;  // "the releaser transferred ownership"
    int queue_hops = 0;    // requeue count
};
```

### 4.3 `AcquireTrace` (active_lock_table.h)

```cpp
struct AcquireTrace {
    bool waited = false;
    float blocking_similarity_score = 0.0f;
    std::string blocking_agent_id;
    int wait_position = 0;   // position in first queue entered
    int wake_count = 0;      // number of CV wakeups
    int queue_hops = 0;      // requeue moves
};
```

### 4.4 Raft Log Entry (dscc_raft.proto)

```protobuf
message LogEntry {
  enum OpType { ACQUIRE = 0; RELEASE = 1; }
  int64 term = 1;
  OpType op_type = 2;
  string agent_id = 3;
  repeated float embedding = 4;
  float theta = 5;
}
```

The Raft log is stored as `std::vector<dscc_raft::LogEntry> log_` in `RaftNode`. Index 0 is a sentinel with term 0. No persistent storage; the entire log is in memory.

### 4.5 Raft RPC Messages (dscc_raft.proto)

**VoteRequest**: `int64 term`, `string candidate_id`, `int64 last_log_index`, `int64 last_log_term`.

**VoteResponse**: `int64 term`, `bool vote_granted`.

**AppendRequest**: `int64 term`, `string leader_id`, `int64 prev_log_index`, `int64 prev_log_term`, `repeated LogEntry entries`, `int64 leader_commit`.

**AppendResponse**: `int64 term`, `bool success`, `int64 conflict_index` (for fast backup on log conflict).

**SnapshotRequest**: `int64 term`, `string leader_id`, `int64 last_included_index`, `int64 last_included_term`, `bytes data`, `int64 offset`, `bool done`.

**SnapshotResponse**: `int64 term`.

**LeaderQuery**: `string from_node`.

**LeaderInfo**: `string leader_id`, `string leader_address`, `bool is_leader`, `int64 current_term`.

### 4.6 Client-Facing RPC Messages (dscc.proto)

**AcquireRequest**: `string agent_id`, `repeated float embedding`, `string payload_text`, `string source_file`, `int64 timestamp_unix_ms`, `OperationType operation_type` (enum: `OPERATION_TYPE_UNSPECIFIED=0`, `OPERATION_TYPE_WRITE=1`, `OPERATION_TYPE_READ=2`).

**AcquireResponse**: `bool granted`, `string message`, `int64 server_received_unix_ms`, `int64 lock_acquired_unix_ms`, `int64 qdrant_write_complete_unix_ms`, `int64 lock_released_unix_ms`, `int64 lock_wait_ms`, `float blocking_similarity_score`, `string blocking_agent_id`, `string leader_redirect`, `string serving_node_id`, `int32 wait_position`, `int32 wake_count`, `int32 queue_hops`, `int32 active_lock_count`.

**PingRequest**: `string from_node`.

**PingResponse**: `string message`.

**ReleaseRequest**: `string agent_id`.

**ReleaseResponse**: `bool released`, `string message`.

### 4.5 Serialization Formats

- **gRPC/Protobuf**: All inter-component communication (client ↔ proxy ↔ node, node ↔ node Raft) uses Protocol Buffers serialized over gRPC. Two proto files: `dscc.proto` (client-facing) and `dscc_raft.proto` (Raft internals).
- **JSON**: Demo inputs (`demo_inputs/*.json`) are hand-crafted JSON. Benchmark output is hand-built JSON (no JSON library; uses manual string concatenation with `escape_json()`).
- **HTTP/JSON**: Qdrant communication uses raw HTTP/1.1 over POSIX sockets with manually constructed JSON bodies. Embedding service communication uses the same raw HTTP approach.
- **CSV**: Matrix mode outputs CSV with headers. Soak mode outputs time-series CSV.

### 4.9 Benchmark Data Structures (benchmark_runner.cpp)

**BenchmarkCase**: `case_index`, `kind` (ScenarioKind enum), `name`, `target` (description), `node_count`, `agent_count`, `write_count`, `read_count`, `theta`, `lock_hold_ms`, `arrival_mode` (kBurst or kStaggered), `arrival_gap_ms`, `collection_name`.

**BenchmarkOperation**: `agent_id`, `role_prefix`, `template_id`, `text`, `embedding` (vector<float>), `operation` (kWrite/kRead), `scheduled_offset_ms`.

**OperationResult**: `operation` (BenchmarkOperation), `status` (grpc::Status), `response` (AcquireResponse), `submit_ms`, `dslm_enter_ms`, `dslm_exit_ms`, `finish_ms`, `elapsed_ms`.

**BenchmarkMetrics**: 35 fields covering total/write/read ops, failures, grants, blocks, conflict pairs, violation counts, parallelism metrics, latency percentiles at p50/p95/p99, lock-wait percentiles, Qdrant window p95, queue statistics.

**ContainerStats**: `name`, `cpu_percent`, `memory_used_mib`, `net_input_mib`, `net_output_mib`. Collected via `docker stats --no-stream`.

**EmbeddingLatencyStats**: `sample_count`, `p50`, `p95`, `p99` — computed from the `embedding_ms` field of `TemplateDocument` samples collected during template loading.

**SoakSnapshot**: `elapsed_sec`, `window_ops`, `total_ops`, `qdrant_size`, lock-wait p50/p95/p99, op-latency p50/p95/p99, `qdrant_window_p95_ms`, `blocked_rate`, `throughput_ops_per_sec`.

**ViolationRecord**: `active_agent_id`, `violating_agent_id`, `active_operation`, `violating_operation`, `similarity`, `detected_unix_ms`, `overlap_start_unix_ms`, `overlap_end_unix_ms`.

**TemplateCatalog**: `concept_a` (first A.json template), `concept_b` (first D.json template), `all` (all templates), `concept_a_all` (all A.json templates including paraphrases).

### 4.10 Demo Input Schema (demo_inputs/*.json)

Each file contains: `agent_name`, `role`, `personality`, `objective`, `payload` (top-level), `scheduled_offset_ms`, `operation` ("write" or "read"), and optionally a `payload_schedule` array. Each schedule entry has `scheduled_offset_ms`, `operation`, and `payload` (text variant). There are 13 input files (A through M), though the primary benchmark uses only A through E.

---

## 5. Control Flow

### 5.1 AcquireGuard Request Lifecycle

1. Client sends `AcquireGuard` RPC to `dscc-proxy:50050`.
2. Proxy calls `RefreshLeader()` (iterates backend nodes via `GetLeader` RPC).
3. Proxy creates `LockService::Stub` for leader address, forwards RPC with `request_timeout_ms_` deadline (default 35000ms).
4. Leader's `LockServiceImpl::AcquireGuard`:
   - Records `server_received_unix_ms`.
   - Validates `agent_id` non-empty, `embedding` non-empty.
   - Checks `raft_->IsLeader()`. If not leader: returns `FAILED_PRECONDITION` with `leader_redirect` in trailing metadata.
   - Calls `lock_table_->wait_for_admission(agent_id, embedding, theta_)`:
     - Waits for `sweep_in_progress_` to clear.
     - Enters loop: `find_conflict_locked()` scans all active locks.
     - If no conflict: `insert_pending_locked()` and break.
     - If conflict: creates `WaitQueueEntry`, attaches to highest-similarity lock's waiter deque, records trace data, waits on per-waiter CV.
     - On wake: if `granted` flag set by releaser, break. Otherwise re-loop.
   - Records `lock_acquired_unix_ms`, `lock_wait_ms`, all trace fields.
   - Constructs `LogEntry{ACQUIRE, agent_id, theta, embedding}`.
   - Calls `raft_->Propose(acquire_entry, timeout, &acquire_log_index)`.
     - Propose appends to local log, then enters replication loop.
     - Spawns one thread per peer for `ReplicateToFollower()`.
     - Each replication thread sends `AppendEntries` RPC.
     - Checks `commit_index_ >= proposed_index` after each replication round.
   - Calls `raft_->WaitUntilApplied(acquire_log_index, timeout)`.
     - Apply loop picks up the committed entry, calls `on_commit_` callback.
     - Callback calls `lock_table.apply_acquire()`, promoting pending → real lock.
   - Performs Qdrant operation (write: `upsert_embedding_to_qdrant`; read: `query_embedding_from_qdrant`).
   - If write and `lock_hold_ms_ > 0`: sleeps.
   - Proposes `RELEASE` via Raft, waits for apply.
   - Apply callback calls `lock_table.apply_release()` → `release()` → `rebalance_waiters_locked()`.
   - Sets `lock_released_unix_ms`, returns response.

### 5.2 Error Paths

- **Propose(ACQUIRE) fails**: Calls `lock_table_->remove_pending(agent_id)`. Appends compensating RELEASE via `AppendLocalEntry()`. Returns `UNAVAILABLE`.
- **WaitUntilApplied(ACQUIRE) times out**: Appends compensating RELEASE. Returns `UNAVAILABLE`.
- **Qdrant write/read fails**: Returns response with `granted=false` and error message. Release still fires via `ScopeExit`.
- **Propose(RELEASE) fails**: `ScopeExit` guard retries release. If retry also fails, logs error.
- **Proxy can't find leader**: Returns `UNAVAILABLE`.
- **NOT_LEADER during RPC**: Proxy extracts `leader-address` from trailing metadata, retries up to 3 times.

### 5.3 Raft Replication Flow

1. Leader appends entry to `log_` with current term.
2. `Propose` spawns threads for parallel replication to all peers.
3. Each `ReplicateToFollower` builds `AppendRequest` with entries from `next_index_[peer]` onward.
4. Sends `AppendEntries` RPC with `config_.rpc_timeout_ms` deadline (default 150ms).
5. On success: updates `match_index_[peer]` and `next_index_[peer]`. Calls `AdvanceCommitIndexLocked()`.
6. On conflict: backs up `next_index_` using `conflict_index` from response (fast backup).
7. On higher term from response: steps down to follower.
8. `AdvanceCommitIndexLocked` scans from log end backward. For each index with term == current_term, counts replicas. If replicated >= quorum, sets `commit_index_`.

### 5.4 Shutdown Sequence

`RaftNode::Stop()`: Sets `running_` to false via CAS. Notifies `commit_cv_` and `apply_cv_`. Joins `election_timer_thread_`, `heartbeat_thread_`, `apply_thread_`.

---

## 6. Data Flow

### 6.1 Data Entry

Data enters the system as natural-language text payloads in `demo_inputs/*.json` files. The benchmark harness or demo binary reads these files, sends the text to the Ollama embedding service via HTTP POST to `/v1/embeddings`, receives a float vector (dimension varies by model: 384 for all-minilm, 1024 for bge-m3 and qwen3-embedding). The embedding vector is then attached to the gRPC `AcquireRequest` and sent to the DSCC cluster.

### 6.2 Transformation Path

1. Raw text → embedding vector (via Ollama HTTP API).
2. Embedding vector → conflict check (cosine similarity against all active lock centroids).
3. If admitted → pending slot in lock table → Raft ACQUIRE log entry → committed across quorum → apply callback promotes to real lock.
4. After lock promotion → Qdrant upsert (write) or search (read).
5. After Qdrant operation → Raft RELEASE log entry → committed → apply callback releases lock → rebalance waiters.

### 6.3 Persistence

- **Qdrant**: Points are persisted with `wait=true` (synchronous write). Each point contains: numeric ID (FNV-1a hash), vector (float array), payload (`agent_id`, `source_file`, `timestamp_unix_ms`, `raw_text`).
- **Raft Log**: Purely in-memory. No WAL, no snapshots. All state is lost on process restart.
- **Lock Table**: Purely in-memory. No persistence.

### 6.4 Data Loss Scenarios

- Process restart: All in-memory Raft log and lock table state is lost. Qdrant data survives.
- Network partition: Minority partition cannot commit; requests block and eventually time out. Majority partition continues operating. On partition heal, minority nodes catch up via AppendEntries.
- Leader crash during Phase 2 (between pending slot and Raft commit): The compensating RELEASE via `AppendLocalEntry` may not propagate if the leader dies before heartbeats replicate it. The new leader's orphan sweep (`begin_leader_sweep`/`end_leader_sweep`) will release stale locks.

---

## 7. Distributed Systems Mechanics

### 7.1 Consensus: Raft

The system uses a custom Raft implementation (not an off-the-shelf library). The implementation covers: leader election, log replication with fast conflict-index backup, commit index advancement (only current-term entries), follower log truncation on conflict, and basic `InstallSnapshot` stub (receives the message and steps down to follower if term is higher, but does not actually install snapshot data).

### 7.2 Leader Election

Standard Raft election with randomized timeout. Candidate increments term, votes for self, sends `RequestVote` RPCs **serially** to all peers (not parallel — this is a potential latency concern with many peers). Requires `QuorumSize()` votes to win. Vote is granted if: (a) voter hasn't voted for another candidate in this term, and (b) candidate's log is at least as up-to-date as voter's log (compared by last log term, then last log index).

### 7.3 Replication

Heartbeat-driven replication with eager replication on `Propose`. Leader maintains per-peer `next_index_` and `match_index_`. Each heartbeat spawns one thread per peer for parallel `AppendEntries` RPCs. RPC timeout is `config_.rpc_timeout_ms` (default 150ms).

### 7.4 Failure Detection

Leader liveness is detected via the election timeout. If a follower does not receive an `AppendEntries` (heartbeat or data) within `[election_timeout_min_ms, election_timeout_max_ms]` (default 600–1000ms), it starts an election. The election timer polls every 10ms.

### 7.5 Split-Brain Mitigation

Standard Raft term-based leader demotion. Any node that receives a message with a higher term steps down to follower. Only one leader per term. The `raft_test.cpp` S7 scenario explicitly verifies at most one leader exists over a 4-second steady-state window.

### 7.6 No Pre-Vote

The system does not implement Raft's Pre-Vote extension. This is acknowledged in `DEMO.md` as a known gap that can cause unnecessary leader disruption when a previously partitioned node rejoins: "gRPC exponential backoff, gRPC channel full, etc. How etcd, CockroachDB, and Consul fix this with Pre-vote. Diego Ongaro in his thesis as an optimization."

---

## 8. Configuration & Tunables

### 8.1 dscc-node Environment Variables

| Variable | Type | Default | Range | Effect |
|---|---|---|---|---|
| `NODE_ID` | string | `"node-1"` | — | Node identifier |
| `PORT` | string | `"50051"` | — | Client-facing gRPC port |
| `RAFT_PORT` | string | `"50061"` | — | Raft inter-node gRPC port |
| `ADVERTISE_HOST` | string | `"127.0.0.1"` | — | Hostname advertised to peers and proxy |
| `PEERS` | CSV string | `""` | — | Comma-separated list of peer Raft addresses |
| `THETA` | float | `0.85` | [0.0, 1.0] | Cosine similarity threshold for conflict detection |
| `LOCK_HOLD_MS` | int | `0` | [0, 600000] | Artificial lock hold duration after Qdrant op |
| `RAFT_PROPOSE_TIMEOUT_MS` | int | `5000` | [50, 600000] | Raft propose deadline |
| `HEARTBEAT_MS` | int | `75` | [10, 5000] | Raft heartbeat interval |
| `ELECTION_TIMEOUT_MIN_MS` | int | `600` | [20, 30000] | Min election timeout |
| `ELECTION_TIMEOUT_MAX_MS` | int | `1000` | [20, 30000] | Max election timeout |
| `RAFT_RPC_TIMEOUT_MS` | int | `150` | [20, 30000] | Per-peer AppendEntries/Vote RPC timeout |
| `QDRANT_HOST` | string | `"qdrant"` | — | Qdrant hostname |
| `QDRANT_PORT` | string | `"6333"` | — | Qdrant port |
| `QDRANT_COLLECTION` | string | `"dscc_memory"` | — | Qdrant collection name |

All are read at startup. None are hot-reloadable.

### 8.2 dscc-proxy Environment Variables

| Variable | Type | Default | Range | Effect |
|---|---|---|---|---|
| `PROXY_PORT` | string | `"50050"` | — | Listen port |
| `BACKEND_NODES` | CSV string | `""` (required) | — | Comma-separated backend node service addresses |
| `LEADER_POLL_MS` | int | `100` | [20, 60000] | Leader polling interval |
| `REQUEST_TIMEOUT_MS` | int | `35000` | [50, 600000] | Per-forwarded-request deadline |
| `LEADER_RPC_TIMEOUT_MS` | int | `750` | [50, 60000] | GetLeader RPC timeout |

### 8.3 docker-compose.yml Defaults

`DSCC_THETA` defaults to `0.78` in the compose file. `DSCC_LOCK_HOLD_MS` defaults to `750`. `QDRANT_COLLECTION` defaults to `dscc_memory_e2e`.

### 8.4 Benchmark Runner Environment Variables

| Variable | Default | Description |
|---|---|---|
| `DSLM_RUN_MODE` | `"single"` | `single`, `matrix`, or `soak` |
| `DSLM_BENCH_OUTPUT` | auto-generated | Single-mode JSON output path |
| `DSLM_MATRIX_OUTPUT` | auto-generated | Matrix CSV output path |
| `DSLM_MATRIX_PROFILE` | all three | Restrict matrix to `ollama`, `bge`, or `qwen` |
| `DSLM_MATRIX_THETA` | all three | Restrict matrix to a single theta |
| `DSLM_SOAK_DURATION_MIN` | `120` | Soak test duration in minutes |
| `DSLM_SOAK_SNAPSHOT_SEC` | `60` | Soak snapshot interval |
| `DSLM_SOAK_THETA` | `0.75` | Soak similarity threshold |
| `DSLM_SOAK_LOCK_HOLD_MS` | `500` | Soak lock hold |
| `E2E_TEARDOWN` | `1` | Tear down Docker stack after run |

---

## 9. Experiments & Benchmarks

### 9.1 Embedding Model Matrix Sweep

**Setup**: 3 models × 3 thetas × 13 scenarios = 117 case runs. Each case tears down and recreates the Docker stack with the appropriate theta and embedding model. Embeddings are computed once per template during stack startup.

**Models**: `all-minilm:latest` (23M params, 384-dim), `bge-m3:latest` (multi-granularity bilingual, 1024-dim), `qwen3-embedding:0.6b` (0.6B params, 1024-dim).

**Thetas**: 0.55, 0.75, 0.95.

**Metrics Measured**: Op latency (p50/p95/p99), lock-wait latency (p50/p95/p99), serialization score, distinct parallelism rate, blocked rate, embedding latency (p50/p95/p99), throughput, Qdrant window p95, write/read latency p95.

**Key Results** (from `MODEL_FINDINGS.md`):

- `qwen3-embedding:0.6b` at θ=0.75 is the only combination achieving serialization score 1.000 on the Paraphrase Gauntlet (Case 11) with zero violations.
- `all-minilm` structurally fails paraphrase detection at all thresholds (best: 0.833 at θ=0.95).
- Embedding overhead: all-minilm p50 ≈ 10ms; bge/qwen p50 ≈ 111–133ms.
- Reader-writer fairness is model-agnostic: Case 13 write/read P95 gap ≤ 60ms across all combinations.

### 9.2 Soak Test

Implemented but not yet run to completion (per `STATE.md`). Design: continuous insertion for configurable duration, windowed latency snapshots. Purpose: verify lock-wait latency stays flat as Qdrant collection grows.

**Per-Round Workload** (from `fire_soak_round`):
- 2 × concept_a writes (sustainability agent — will serialize with each other)
- 1 × concept_a read (may queue behind writes)
- 1 × concept_b write (construction agent — distinct domain, always free)
- 1 × concept_b read (distinct domain read)
- All 5 operations fire as burst (all threads start simultaneously).

**Snapshot Collection**: Every `soak_snapshot_sec` (default 60s), the runner queries `qdrant_point_count` to track collection size, and computes windowed percentiles for lock-wait, op-latency, and Qdrant window metrics.

**Output**: CSV file with columns: `elapsed_sec`, `window_ops`, `total_ops`, `qdrant_size`, `lock_wait_p50_ms`, `lock_wait_p95_ms`, `lock_wait_p99_ms`, `op_latency_p50_ms`, `op_latency_p95_ms`, `op_latency_p99_ms`, `qdrant_window_p95_ms`, `blocked_rate`, `throughput_ops_per_sec`.

**Configuration**: `DSLM_SOAK_DURATION_MIN` (default 120 min), `DSLM_SOAK_SNAPSHOT_SEC` (default 60s), `DSLM_SOAK_THETA` (default 0.75), `DSLM_SOAK_LOCK_HOLD_MS` (default 500ms), `DSLM_SOAK_OUTPUT` (auto-generated path).

### 9.3 Paraphrase Gauntlet Demo

Standalone binary (`dscc-paraphrase-gauntlet-demo`) that produces a confusion matrix per model, computing accuracy, precision, recall, F1, serialization score, distinct parallelism rate, and false positive rate. Outputs JSON to `logs/paraphrase_gauntlet_results_<timestamp>.json`.

### 9.4 Locust Workload Generators

Two Locust files for overhead comparison:

**`locustfile.py`** (Full DSLM gRPC traffic):
- `DSLMUser` class extends `locust.User`.
- Each user is assigned a round-robin persona from the filtered set (default A–E via `DSCC_AGENT_LABELS`).
- Each request builds a `dscc.AcquireRequest` with pre-computed embeddings, sends via `LockServiceStub.AcquireGuard`.
- Fires per-user at `OP_INTERVAL_MS` ± `OP_JITTER_MS` (defaults 1000 ± 200ms).
- `USE_FIRST_PAYLOAD_ONLY=1` (default) restricts each persona to its first payload for maximal conflict density.
- Proto stubs are lazy-imported (`dscc_pb2`, `dscc_pb2_grpc`); requires manual generation via `grpc_tools.protoc`.
- gRPC channel is created once per user (`grpc.insecure_channel`), reused across requests.
- Reports results as `request_type="grpc"` with name format `AcquireGuard/<label>/<operation>`.

**`locustfile_base.py`** (Direct Qdrant HTTP baseline):
- `QdrantBaselineUser` class extends `locust.User`.
- Same persona assignment, same pacing, same embeddings as `locustfile.py`.
- Writes: `PUT /collections/{col}/points?wait=true` with one point (id, vector, payload).
- Reads: `POST /collections/{col}/points/search` with `limit=3, with_payload=false, with_vector=false`.
- Uses `requests.Session()` for connection pooling per user.
- Reports results as `request_type="http"` with name format `QdrantDirect/<label>/<operation>`.
- Manages Qdrant collection lifecycle: can delete+recreate on startup (`BASELINE_RESET_COLLECTION=1`).

**`scripts/compare_baseline.sh`**:
- Runs both Locust files sequentially: baseline first, then DSLM.
- Same workload knobs for both runs (agent labels, interval, jitter, first-payload-only).
- Supports headless mode or WATCH mode (live Locust web UI on port 8089).
- Calls `plot_overhead.py` to produce a side-by-side comparison plot and `overhead_summary.csv`.
- Duration, users, spawn rate, output directory are all configurable via flags and environment variables.

### 9.5 Thundering Herd Script

**`thundering_herd.py`**: Standalone Python workload generator for targeted burst testing.
- Mirrors benchmark Case 1 (`ScenarioKind::kThunderingHerd`) exactly.
- Uses `threading.Barrier(N)` to synchronize all N agents, then fires simultaneously.
- All agents use the same embedding (from A.json's first payload), guaranteeing `cosine >> theta` between every pair.
- Configurable via environment: `HERD_AGENTS` (default 10), `HERD_WAVES` (default 5, 0=infinite), `HERD_WAVE_GAP_S` (default 3.0), `HERD_TIMEOUT_S` (default 60).
- Prints per-wave result table with columns: Agent, Finish time, wait_position, queue_hops, wake_count, lock_wait_ms, total elapsed_ms.
- Prints aggregate stats: max queue depth, max hops, avg lock_wait, span, throughput.

### 9.6 Overhead Comparison Methodology

The coordination overhead is defined as:
```
coordination_overhead = latency(locustfile.py) - latency(locustfile_base.py)
```
This isolates the cost of the DSLM stack (gRPC proxy hop, leader forwarding, Raft replication, lock-table bookkeeping, lock-hold time) from the baseline Qdrant latency. The documentation in `locustfile_base.py` notes two caveats: (1) DSLM writes internally perform one search + one upsert, while baseline writes only do one upsert; (2) DSLM response time includes `LOCK_HOLD_MS` (750ms default). For pure coordination-cost comparison, the documentation recommends rerunning with `DSCC_LOCK_HOLD_MS=0`.

---

## 10. Metrics & Observability

### 10.1 AcquireResponse Trace Fields

Every `AcquireGuard` response contains: `server_received_unix_ms`, `lock_acquired_unix_ms`, `qdrant_write_complete_unix_ms`, `lock_released_unix_ms`, `lock_wait_ms`, `blocking_similarity_score`, `blocking_agent_id`, `leader_redirect`, `serving_node_id`, `wait_position`, `wake_count`, `queue_hops`, `active_lock_count`.

### 10.2 Live Queue Events

Emitted via `log_line()` from `active_lock_table.cpp`:
- `[LOCK_QUEUE] agent=X waiting_on=Y similarity=Z queue_position=N theta=T`
- `[LOCK_REQUEUE] agent=X waiting_on=Y similarity=Z queue_position=N queue_hops=H theta=T`
- `[LOCK_GRANT] agent=X queue_hops=H active_locks=N`

### 10.3 Health Checks

`grpc::EnableDefaultHealthCheckService(true)` is enabled in both `main.cpp` and `proxy_main.cpp`.

### 10.4 Logging

Ad-hoc `std::cout` via `log_line()`. Not structured. Not sampled. `TODO.md` acknowledges: "Replace ad-hoc `std::cout` with structured logging format."

---

## 11. Performance Characteristics & Claims

### 11.1 Quantitative Claims (from MODEL_FINDINGS.md)

- `qwen3-embedding:0.6b` at θ=0.75: serialization score 1.000 on Paraphrase Gauntlet, 0 violations.
- `all-minilm` embedding: p50 ≈ 10ms steady-state.
- `bge`/`qwen` embedding: p50 ≈ 111–133ms steady-state.
- System-level latency at θ=0.75: P95 ≈ 2117–2145ms across all models.
- Reader-writer P95 gap: ≤ 60ms across all (model, θ) combinations.

### 11.2 Algorithmic Complexity

- Conflict detection: O(n) per admission, where n = number of active locks. Linear scan with cosine similarity computation per lock.
- Cosine similarity: O(d) per pair, where d = embedding dimension (384 or 1024).
- Raft commit index advancement: O(log_size × peer_count) per advancement check.
- Waiter rebalancing: O(w × n) per release, where w = number of waiters on the released lock.

### 11.3 Identified Bottlenecks

- Embedding latency is an irreducible per-operation cost (10ms for all-minilm, ~130ms for qwen).
- Lock hold time (`LOCK_HOLD_MS`) is configurable artificial latency. Set to 0 for pure coordination-cost measurement.
- Serial vote collection during elections (not parallel).

---

## 12. Correctness & Testing

### 12.1 Test Files

| File | Scope | Docker | gRPC | Qdrant |
|---|---|---|---|---|
| `testbench.cpp` | In-process lock table | No | No | No |
| `raft_test.cpp` | In-process Raft (3 nodes) | No | Yes (localhost) | No |
| `e2e_bench.cpp` | Full stack E2E | Yes | Yes | Yes |
| `benchmark_runner.cpp` | Curated benchmark | Yes | Yes | Yes |
| `paraphrase_gauntlet_demo.cpp` | Model comparison | Yes | Yes | Yes |

### 12.2 Correctness Metric

For every pair of operations: if `cosine_similarity(embed_i, embed_j) >= theta`, the pair is an "expected conflict pair." If their lock intervals `[lock_acquired_unix_ms, lock_released_unix_ms)` overlap, it's a "conflicting overlap violation." `serialization_score = 1 - (violations / expected_conflict_pairs)`.

### 12.3 Known Test Coverage Gaps

From `TODO.md`:
- No failure-path tests (Qdrant down, bad vector size, invalid payloads).
- No deterministic tests for timeout and retry behavior.
- No property-based or fuzz tests.
- Some benchmark scenarios still report occasional correctness violations (Thundering Herd, Semantic Interleaving, Read-Starvation Trap, Permissive Sieve, Queue Hopping, 100% Read Stampede).

---

## 13. Error Handling & Edge Cases

### 13.1 Error Types

- `grpc::FAILED_PRECONDITION` with message "NOT_LEADER" — non-leader node received a client request.
- `grpc::UNAVAILABLE` — "Raft quorum not reached for ACQUIRE" or "Raft quorum not reached for RELEASE".
- Application-level `granted=false` — "agent_id is required", "embedding is required", "qdrant write failed", "qdrant read failed".
- `std::runtime_error` in benchmark harness — Docker failures, HTTP failures, timeout waiting for services.

### 13.2 Edge Cases Handled

- **Ghost ACQUIRE**: If `Propose(ACQUIRE)` fails after a pending slot is created, `remove_pending` cleans up locally and a compensating `RELEASE` is appended via `AppendLocalEntry` to cancel any partially replicated ACQUIRE on follower logs.
- **Release-on-error**: `ScopeExit` guard in `AcquireGuard` ensures `commit_release_once()` is called even if Qdrant fails or the function exits early.
- **Leader change during sweep**: `begin_leader_sweep` gates `wait_for_admission` calls. The sweep loop checks `raft.IsLeader()` before each orphan release and aborts if leadership is lost.
- **Duplicate agent_id**: `apply_acquire_locked` and `insert_pending_locked` overwrite if the agent_id already exists in the active map.
- **Qdrant collection race**: `ensure_qdrant_collection` handles 200, 201, 409, and 400-with-"already exists" responses.
- **Empty/mismatched embeddings**: `cosine_similarity` returns 0.0f if vectors are empty or have different sizes.

---

## 14. Design Decisions & Tradeoffs

### 14.1 Raft-First with Pending/Promote

The system uses a two-step admission model: `wait_for_admission` inserts a **pending** slot that participates in conflict detection before Raft commit. This prevents any request from slipping through between admission and commit. The tradeoff: the leader holds a semantic lock locally before durable cluster agreement. This is acknowledged in `STATE.md` §7.3 as "the first thing to revisit if correctness behavior becomes the main priority."

### 14.2 C++ Proxy Instead of Go

`VERSION_1.md` originally planned a Go proxy. The actual implementation uses C++ (`src/proxy_main.cpp`). Rationale stated in `STATE.md` §4.2: "the decision was made to keep the entire stack in C++ for build simplicity and a single container image."

### 14.3 5-Node Cluster Instead of 3

`VERSION_1.md` planned for 3 nodes. The implementation uses 5 nodes (quorum = 3, tolerates 2 failures). This is noted in the implementation note at the top of `VERSION_1.md`.

### 14.4 No External Dependencies for HTTP

Qdrant communication uses raw POSIX sockets rather than a maintained HTTP client library. `TODO.md` §3 acknowledges: "Replace raw socket HTTP with a maintained HTTP client library."

### 14.5 No JSON Library

All JSON parsing and generation is manual string manipulation. No `nlohmann/json`, no `rapidjson`, no external JSON library.

### 14.6 Per-Waiter Condition Variables

Each waiter gets its own `shared_ptr<condition_variable>`. This allows targeted wakeup of individual waiters during rebalancing, avoiding thundering-herd effects on the wait side.

### 14.7 Reads Through Semantic Locking

`STATE.md` §2.2: "the current read path is conservative because reads also go through semantic locking." Reads are blocked by active writes with similar embeddings. This was a deliberate correctness-over-throughput choice.

---

## 15. Related Works, Inspirations & References

### 15.1 Raft Consensus

The implementation follows the Raft protocol as described by Diego Ongaro. `DEMO.md` explicitly references "Diego Ongaro in his thesis as an optimization" when discussing the missing Pre-Vote mechanism. The Raft implementation covers leader election, log replication, commit index advancement, and fast log backup on conflict. Log truncation behavior is tested in `raft_test.cpp` S8.

### 15.2 Qdrant

Used as the vector persistence and search backend. Communicates via HTTP REST API. Collection distance metric is always "Cosine".

### 15.3 Ollama

Used as the embedding service. Serves all three models (`all-minilm:latest`, `bge-m3:latest`, `qwen3-embedding:0.6b`) from a single container. Model selection is per-request via the API `model` field.

### 15.4 Systems Referenced in DEMO.md

etcd, CockroachDB, and Consul are referenced as systems that implement Pre-Vote to prevent disruption from rejoining partitioned nodes.

---

## 16. Dependencies & Third-Party Integrations

### 16.1 Build Dependencies (CMakeLists.txt)

- **CMake** >= 3.16
- **Protobuf** (found via CMake config or `find_package`)
- **gRPC** (found via CMake config or pkg-config)
- **Threads** (pthreads)
- C++17 standard for `dscc-node`, `dscc-testbench`, `dscc-proxy`. C++20 for `dscc-e2e-bench`, `dscc-benchmark`, `dscc-e2e-demo`, `dscc-paraphrase-gauntlet-demo` (for `std::barrier`).

### 16.2 Docker Dependencies (docker/Dockerfile)

Base image: `ubuntu:22.04`. Installed packages: `build-essential`, `cmake`, `pkg-config`, `protobuf-compiler`, `protobuf-compiler-grpc`, `libprotobuf-dev`, `libgrpc++-dev`, `libgrpc-dev`. No pinned versions.

### 16.3 Runtime Dependencies (docker-compose.yml)

- `qdrant/qdrant` (no version pin)
- `ollama/ollama:latest` (configurable via `EMBEDDING_IMAGE`)

### 16.4 Python Dependencies (locustfile.py, scripts)

- `locust` — workload generation
- `grpcio`, `grpcio-tools` — gRPC client and proto compilation
- `requests` — HTTP client for embedding service
- `matplotlib`, `numpy`, `pandas` — plotting scripts

---

## 17. Build, Deployment & Operations

### 17.1 Build System

CMake with two proto files generating both protobuf and gRPC stubs into `${CMAKE_CURRENT_BINARY_DIR}/generated`. The `dscc_add_proto` function generates `.pb.cc`, `.pb.h`, `.grpc.pb.cc`, `.grpc.pb.h` per proto file. A static library `dscc_proto` is built from all generated sources.

**Build targets**: `dscc-node`, `dscc-proxy`, `dscc-testbench`, `dscc-e2e-bench`, `dscc-benchmark`, `dscc-e2e-demo`, `dscc-paraphrase-gauntlet-demo`, `dscc-raft-test`.

### 17.2 Docker Build

The Dockerfile copies the entire source tree, runs `cmake -S . -B build` and `cmake --build build` inside the container. The resulting image contains all compiled binaries. Docker Compose selects the appropriate binary via the `command` directive.

### 17.3 Operational Procedures

**Start full stack**: `docker compose up -d --build qdrant embedding-service dscc-node-1 dscc-node-2 dscc-node-3 dscc-node-4 dscc-node-5 dscc-proxy`

**Run single benchmark**: `E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark`

**Run matrix sweep**: `DSLM_RUN_MODE=matrix E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark`

**Run soak test**: `DSLM_RUN_MODE=soak E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark`

**Manual agent request**: `./agent_request.sh write "Review the massing concept"` (requires `grpcurl` and `jq`).

### 17.4 No CI/CD Pipeline

`TODO.md` §6: "Add CI build + test pipeline" is unchecked.

---

## 18. Known Bugs, Limitations & Open Issues

### 18.1 TODO/FIXME/HACK/NOTE Comments

**`src/e2e_bench.cpp` line 829**:
```
// NOTE, we need to change this function so that it uses the Config struct to get the service name for the node id.
```
Followed by an inline comment block:
```
/*
This actually doesn't use config at all and the logic is correct — it just prepends "dscc-" to "node-4" to get "dscc-node-4".
*/
```

**`src/e2e_bench.cpp` line 64**:
```cpp
// this is currently hardcoded to the testbench. We need to make sure we dynamically populate this map.
```
Referring to `node_targets` and `node_service_names` arrays.

### 18.2 TODO.md — Open Items (Verbatim)

**§1 Correctness**:
- `[ ]` "Enforce lock ownership on release (only lock owner can release)."
- `[x]` "Prevent duplicate active locks for the same `agent_id`." *(apply_acquire_locked and insert_pending_locked overwrite if agent_id already present)*
- `[ ]` "Validate embedding dimensions across requests before lock acquisition."
- `[ ]` "Decide and enforce semantic overlap rule (`incoming threshold` vs `per-lock threshold`)."
- `[ ]` "Add bounded waiting / timeout support for blocked acquisitions."

**§2 API and Service Behavior**:
- `[ ]` "Add explicit error codes in gRPC responses (invalid input, timeout, write failure)."
- `[ ]` "Add request idempotency behavior for retries."
- `[ ]` "Add an explicit 'acquire-only' mode vs current 'acquire + write + release' RPC behavior."

**§3 Qdrant Integration**:
- `[ ]` "Replace raw socket HTTP with a maintained HTTP client library."
- `[ ]` "Parse HTTP response body and surface detailed write errors."
- `[x]` "Add retry policy with backoff for transient Qdrant failures." *(upsert_embedding_to_qdrant retries up to 3 times with 75ms × attempt backoff)*
- `[ ]` "Add startup health check for Qdrant availability."

**§4 Testing**:
- `[x]` "Add unit tests for `ActiveLockTable` overlap and wait behavior." *(testbench.cpp — 5 concurrency scenarios)*
- `[x]` "Add integration tests for gRPC `AcquireGuard`/`ReleaseGuard`." *(raft_test.cpp — 7 suites; e2e_bench.cpp — full-stack scenarios; benchmark_runner.cpp — 13 curated cases)*
- `[ ]` "Add failure-path tests (Qdrant down, bad vector size, invalid payloads)."
- `[ ]` "Add deterministic tests for timeout and retry behavior."

**§5 Observability**:
- `[ ]` "Replace ad-hoc `std::cout` with structured logging format."
- `[x]` "Add metrics: active lock count, wait time, acquisition latency, Qdrant write latency." *(AcquireResponse exposes lock_wait_ms, active_lock_count, queue_hops, wake_count, blocking_similarity_score; benchmark_runner emits per-case and matrix CSV metrics)*
- `[x]` "Add lock contention counters and conflict-rate reporting." *(LOCK_QUEUE / LOCK_REQUEUE / LOCK_GRANT log events with similarity, queue position, hops; benchmark_runner computes blocked_rate, serialization_score, conflicting_overlap_violations)*

**§6 Deployment**:
- `[x]` "Add a simple health endpoint/readiness strategy for `dscc-node`." *(grpc::EnableDefaultHealthCheckService(true) in both main.cpp and proxy_main.cpp)*
- `[ ]` "Add CI build + test pipeline."
- `[x]` "Add runtime configuration validation at startup (port, theta, qdrant host/port)." *(read_int_from_env / read_float_from_env with min/max bounds; proxy checks for empty BACKEND_NODES)*

**§7 Distributed SLM**:
- `[x]` "Shared active lock state across nodes." *(Raft-replicated ACQUIRE/RELEASE log entries; apply callback syncs ActiveLockTable on all replicas)*
- `[x]` "Node-to-node coordination protocol (leader/follower or consensus)." *(Custom Raft over gRPC — leader election, log replication, commit/apply sequencing via dscc_raft.proto)*
- `[ ]` "Lease/heartbeat handling for lock expiration." *(Raft heartbeats exist for leader liveness, but no lease-based expiry for abandoned locks)*
- `[x]` "Partition handling and recovery semantics." *(Raft quorum prevents isolated leader from committing; follower catch-up via AppendEntries on rejoin)*
- `[x]` "Distributed contention control and fairness policy." *(Per-lock FIFO waiter queues with requeue and hop tracking; leader-only admission via wait_for_admission)*

### 18.3 Known Correctness Issues

Per `STATE.md` §12.1: Some scenarios still report occasional correctness violations: The Thundering Herd, The Semantic Interleaving, The Read-Starvation Trap, The Permissive Sieve, Queue Hopping, The 100% Read Stampede.

Per `STATE.md` §7.3: "Local leader admission happens before durable cluster agreement on ACQUIRE. So the current design has this property: the leader may hold a semantic lock locally before the acquire is fully committed. That is a meaningful architectural caveat."

### 18.4 Incomplete Features

- Soak test has not been run to completion (2-hour session).
- `InstallSnapshot` RPC is a stub — it receives the message, steps down to follower if term is higher, but does not install actual snapshot data.
- No persistent WAL or snapshot mechanism for Raft log.
- No Pre-Vote mechanism.

### 18.5 Commit History (Most Recent 30)

```
62674c2 Merge pull request #7 from ashwatthaphatak/distributed_demo
f88321c added thundering herd testcase
5235cfe change DEMO.md file
828935c adding explicit commands for test cases 2,3,4
50eb1e3 modified agent workloads to better suit demos for paraphrase gauntlet
34b90e4 modified Agent A's workload to be more paraphrased for gauntlet demo test
4ffe08d adding demo readme
277f959 distribute
9726d0d added demo for calculating latency overhead with and without the DSLM
52cafba added demo testcase for embedding model comparison and paraphrase detection
cc7c7a6 added new multiple agent profiles for longer running workloads
ca5c9f9 added a shell file that will allow us to send individual requests to a system
781c640 Fixed Raft's leader failover methods
6eccf91 initial demo implementation
e27199d fixed raft tests
46d1710 fixed testbench.cpp to follow new apply_release callbacks
ef639a2 corrected inconsistent docs and comments
9133784 Merge remote-tracking branch 'origin/feature/embedding_models' into v1.0
18956be Merge pull request #5 — fix v1.0 e2ebench validation logic
5c6dfc5 Merge pull request #4 — fix-raft-logic
ce0a036 fixed RAFT testbench and created new docs for RAFT paths and flow
cc6930a added fix for raft first admission. Updated RAFT docs
9330b2f fixed ghost lock problem by using a compensating RELEASE
6607659 Updated markdowns
13ab0d3 Updated plots with MODEL_FINDINGS.md for reference
d998e09 Merge v1.0: RAFT consensus, readers-writer semantics, benchmark runner, JSON demo inputs
3c8fab5 CLAUDE.md
2dc9eee modded gitignore
50c2623 Adding more Embedding Models
41d63c1 added explanation markdown files for both versions, active lock table, architecture
```

### 18.6 Version History

**VERSION_0** (`expl_docs/VERSION_0.md`): Documents the initial single-node architecture. Key characteristics: one `dscc-node` process, no Raft, no proxy, single `ActiveLockTable`. This version implemented the core semantic lock concept but lacked distribution.

**VERSION_1** (`expl_docs/VERSION_1.md`): Plans the replicated architecture. Originally designed for a 3-node cluster with a Go proxy. The actual implementation diverged to a 5-node cluster with a C++ proxy, as noted in the implementation note at the top of the document. This document served as the architectural blueprint for the Raft integration.

### 18.7 Documentation Files

| File | Purpose |
|---|---|
| `README.md` | Build instructions, architecture overview, demo/benchmark usage |
| `STATE.md` | Authoritative current-state description of the repository |
| `TODO.md` | Acknowledged bugs, limitations, and open issues |
| `MODEL_FINDINGS.md` | Empirical results from the embedding model evaluation |
| `DEMO.md` | Instructions for running the live presentation demo |
| `CLAUDE.md` | AI model context document |
| `expl_docs/VERSION_0.md` | Historical: single-node architecture snapshot |
| `expl_docs/VERSION_1.md` | Historical: distributed architecture plan |
| `expl_docs/RAFT_EXPL.md` | Raft paths and flow explanation |
| `expl_docs/ACTIVE_LOCK_TABLE.md` | ActiveLockTable internals explanation |
| `expl_docs/DETAILED_ARCHITECTURE_DIAGRAM.md` | Detailed architecture diagrams |
| `expl_docs/PRESENTATION.md` | Mid-review presentation content |

### 18.8 License

MIT License. Copyright (c) 2026 Ashwattha Phatak, Ayush Gala.
