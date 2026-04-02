# DSCC — Distributed Semantic Conflict Controller
## Mid-Review Technical Presentation

---

## Slide 1: Title

**Distributed Semantic Conflict Controller (DSCC)**
A Raft-Replicated Semantic Lock Manager for Multi-Agent Vector Memory Systems

Ashwattha Phatak, Ayush Gala
Mid-Review — April 2026

---

## Slide 2: Problem Statement

Multi-agent systems that share a vector memory (e.g., Qdrant) can issue concurrent writes whose **intent** overlaps even when the writes target different keys or rows. Traditional lock managers detect conflict by key identity; they cannot detect that "Process the Apple invoice for March" and "Handle Apple invoice processing for March" are semantically the same operation.

**Core problem:** Without semantic conflict detection, two agents can concurrently mutate overlapping regions of a shared knowledge base, producing inconsistent or contradictory state.

**Our goal:** Build a layer that sits between agents and the vector database, detects semantic conflicts via cosine similarity on embedding vectors, and serializes conflicting operations — while allowing unrelated operations to proceed in parallel.

---

## Slide 3: What is Semantic Locking?

- Every request carries a natural-language payload and its embedding vector (384-dim, `all-minilm`)
- Before a write or read proceeds, the system compares the incoming embedding against all currently active lock centroids
- If `cosine_similarity(incoming, active) >= θ`, the request is **semantically conflicting** and must wait
- If similarity is below θ for all active locks, the request proceeds in parallel
- θ is a configurable threshold:
  - 0.95 → only near-identical texts conflict
  - 0.78 → strong paraphrases conflict (our default)
  - 0.30 → loosely related texts conflict

**This gives us semantic serializability**: conflicting operations are serialized while non-conflicting operations execute concurrently.

---

## Slide 4: Concrete Example — Our Demo Workload

We designed five agent personas from an architecture/construction domain:

| Agent | Role | Payload (first scheduled) |
|-------|------|---------------------------|
| A (Ari) | Sustainability Agent | "Review the massing concept and prioritize passive cooling, daylight access, and low-carbon material options for the civic annex." |
| B (Blake) | Safety Agent | "Check the massing design against fire egress codes and flag any enclosures that limit safe evacuation routes." |
| C (Cameron) | Cost Agent | "Estimate cost ranges for the structural grid options and flag any specification that exceeds the target per-square-foot budget." |
| D (Dana) | Construction Agent | "Confirm the proposed payroll structure covers all subcontractor tiers and flag any compliance gaps before the next draw." |
| E (Emery) | Client Agent | "Verify that the payroll submission aligns with the current contract terms and highlight any items that need client approval." |

**Conflict pairs at θ=0.78:** A↔B (~0.85–0.90 similarity, both about building massing/design), D↔E (~0.88–0.92 similarity, both about payroll). Agent C is semantically distinct from all others.

Each agent also has a `payload_schedule` — a stream of multiple timed operations (writes and reads) used by the curated benchmark.

---

## Slide 5: Phase 1 — Single-Node Prototype Architecture

```
┌───────────────────┐       ┌────────────────────┐       ┌───────────────┐
│    e2e_bench       │       │     dscc-node       │       │    Qdrant     │
│    (host, C++)     │──────▶│   (Docker, C++)     │──────▶│  (Docker)     │
│                    │ gRPC  │                     │ HTTP  │               │
│ Reads demo_inputs  │       │  ActiveLockTable    │       │ Vector DB     │
│ Calls Ollama for   │       │  LockServiceImpl    │       │ Port 6333     │
│ embeddings         │       │  Port 50051         │       │               │
└────────┬───────────┘       └─────────────────────┘       └───────────────┘
         │ HTTP
         ▼
┌────────────────────┐
│  Ollama (Docker)   │
│  all-minilm:latest │
│  Port 7997→11434   │
│  384-dim embeddings│
└────────────────────┘
```

**Components:**
1. **Ollama** — embedding service; converts text → 384-dimensional float vectors
2. **dscc-node** — single C++ gRPC server; in-memory `ActiveLockTable` + raw-socket HTTP Qdrant writer
3. **Qdrant** — vector database for persistent memory storage
4. **e2e_bench** — host-side test harness orchestrating Docker, embeddings, scenarios, validation

---

## Slide 6: Single-Node Lock Table — Original Design

```cpp
class ActiveLockTable {
    std::vector<SemanticLock> active_;    // all currently held locks
    std::mutex mu_;                       // protects active_
    std::condition_variable cv_;          // single global CV for all waiters
};
```

**Acquire algorithm:**
1. Lock `mu_`
2. Scan all entries in `active_` — compute cosine similarity against incoming embedding
3. If any `similarity >= θ` → `cv_.wait(lock)`, re-check on wake
4. If no conflict → `active_.push_back(...)`, return

**Release algorithm:**
1. Lock `mu_`, remove entry, unlock
2. `cv_.notify_all()` — wake **every** blocked thread

**Problems with this design:**
- **Thundering herd**: every release wakes all N blocked agents; N-1 immediately re-sleep
- **No fairness**: FIFO ordering not guaranteed; starvation possible under sustained contention
- **No per-lock locality**: agents blocked on unrelated locks wake and re-scan unnecessarily

---

## Slide 7: Single-Node Lock Table — Redesigned with Per-Lock Wait Queues

```cpp
struct WaitQueueEntry {
    std::string waiting_agent_id;
    std::vector<float> embedding;
    float theta;
    std::shared_ptr<std::condition_variable> cv;    // per-waiter CV
    bool ready = false;
    bool granted = false;
    int queue_hops = 0;
};

struct SemanticLock {
    std::string agent_id;
    std::vector<float> centroid;
    float threshold;
    std::deque<std::shared_ptr<WaitQueueEntry>> waiters;  // per-lock FIFO queue
};

class ActiveLockTable {
    std::unordered_map<std::string, SemanticLock> active_;   // O(1) release lookup
    std::mutex mu_;
};
```

**Key changes:**
- Replaced global `std::condition_variable` with per-waiter CVs attached to per-lock queues
- Replaced `std::vector<SemanticLock>` with `std::unordered_map<string, SemanticLock>` for O(1) release
- Added `WaitQueueEntry` with `ready`/`granted` flags to support ownership handoff during release
- Added `queue_hops` counter to track how many times a waiter gets requeued behind different locks

---

## Slide 8: Per-Lock Queue — Release and Rebalance

When a lock is released, its waiter queue must be **rebalanced**:

```
RELEASE(agent_id):
  lock(mu_)
  waiters = remove active lock, take its waiter deque
  
  FOR EACH waiter (front to back):
    conflict = find_conflict(waiter.embedding, waiter.theta)
    IF conflict exists:
      conflict.waiters.push_back(waiter)    // requeue behind new blocker
      waiter.queue_hops++                   // track the hop
      emit [LOCK_REQUEUE] log event
    ELSE:
      apply_acquire(waiter)                 // grant immediately
      waiter.granted = true                 // handoff: no re-check needed
      waiter.ready = true
      emit [LOCK_GRANT] log event
      collect waiter for notification
  unlock(mu_)
  
  notify all granted waiters (outside lock)
```

**Properties:**
- Only agents that can actually make progress are woken — no thundering herd
- FIFO ordering within each conflict group
- Queue hops = number of times a waiter moved to a different lock's queue before being granted
- High hop count = queue churn, potential fairness pressure under sustained multi-way contention

---

## Slide 9: The Single Point of Failure Problem

The single-node prototype had one critical weakness: **if the dscc-node process crashes, all in-memory lock state is lost and no agent can proceed.**

We explored several replication architectures:

| Architecture | Pros | Cons |
|-------------|------|------|
| **Active-Passive with shared storage** | Simple failover | Storage becomes the SPOF; lock table is in-memory, not easily shared |
| **Primary-Backup with state shipping** | Low complexity | No strong consistency guarantee during failover; split-brain risk |
| **Read-serving follower nodes** | Read scalability | Stale reads create semantic violations; a read that doesn't see a recent write's lock can proceed when it shouldn't |
| **Raft consensus with leader-only admission** | Strong consistency; automatic leader election; replicated log | Higher implementation complexity; write latency includes quorum round-trip |

**Decision:** Raft consensus. The read-serving-followers approach was rejected because **semantic locking requires all admission decisions to see the same lock state** — a follower serving reads with a slightly stale view can grant a conflicting read, violating the semantic serializability invariant.

---

## Slide 10: Distributed Architecture — Current System

```
                     Agents / Benchmark
                            │
                            ▼
                   ┌─────────────────┐
                   │   dscc-proxy    │  Port 50050
                   │  Leader-aware   │  Forward + retry
                   │  C++ gRPC proxy │
                   └──┬──┬──┬──┬──┬─┘
                      │  │  │  │  │
        ┌─────────────┘  │  │  │  └────────────────┐
        │     ┌──────────┘  │  └──────────┐        │
        ▼     ▼             ▼             ▼        ▼
   ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
   │ node-1  │ │ node-2  │ │ node-3  │ │ node-4  │ │ node-5  │
   │ :50051  │ │ :50052  │ │ :50053  │ │ :50054  │ │ :50055  │
   │ Raft    │ │ Raft    │ │ Raft    │ │ Raft    │ │ Raft    │
   │ :50061  │ │ :50062  │ │ :50063  │ │ :50064  │ │ :50065  │
   └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘
        │           │           │           │           │
        └───────────┴─────┬─────┴───────────┴───────────┘
                          │ Full mesh Raft RPC
                          ▼
                   ┌──────────────┐          ┌──────────────┐
                   │   Qdrant     │          │  Ollama      │
                   │   Port 6333  │          │  Port 7997   │
                   └──────────────┘          └──────────────┘
```

**5 dscc-node processes** — each runs `ActiveLockTable` + `RaftNode` + `LockServiceImpl` + `RaftServiceImpl`

**1 dscc-proxy** — polls `GetLeader` across all nodes, forwards RPCs to current Raft leader, retries on NOT_LEADER / UNAVAILABLE / DEADLINE_EXCEEDED (up to 3 attempts)

Originally designed with 3 nodes; expanded to **5 nodes** for better fault tolerance (tolerates 2 simultaneous failures, quorum = 3).

---

## Slide 11: Raft Integration — What We Replicate

The Raft log replicates **lock lifecycle transitions**, not the lock table state itself:

```protobuf
message LogEntry {
  enum OpType { ACQUIRE = 0; RELEASE = 1; }
  int64 term = 1;
  OpType op_type = 2;
  string agent_id = 3;
  repeated float embedding = 4;    // only for ACQUIRE
  float theta = 5;                 // only for ACQUIRE
}
```

Each `AcquireGuard` RPC produces exactly two log entries:
1. **ACQUIRE** — proposed after the leader grants the local lock
2. **RELEASE** — proposed after Qdrant operation + hold time completes

**Raft apply callback on every node:**
```cpp
if (entry.op_type() == ACQUIRE)
    lock_table.apply_acquire(agent_id, embedding, theta);
else
    lock_table.apply_release(agent_id);
```

Followers reconstruct lock table state by replaying committed log entries. They never run conflict checks; they simply mirror the leader's admission decisions.

---

## Slide 12: Raft Implementation Details

**`RaftNode` class** — custom C++ implementation, ~800 lines:

| Component | Details |
|-----------|---------|
| **States** | FOLLOWER → CANDIDATE → LEADER (standard Raft FSM) |
| **Election** | Randomized timeout [600ms, 1000ms]; `RequestVote` RPC; log-completeness check (`IsLogUpToDateLocked`) |
| **Heartbeat** | 75ms interval; empty `AppendEntries` maintains leader authority |
| **Replication** | `ReplicateToFollower()` sends `AppendEntries` with log entries, `prev_log_index/term` for consistency |
| **Commit** | `AdvanceCommitIndexLocked()` — majority `match_index` determines `commit_index` |
| **Apply** | Dedicated `ApplyLoop` thread; calls `on_commit_` callback when `last_applied < commit_index` |
| **Quorum** | `(total_nodes + 1) / 2` = 3 for a 5-node cluster |

**RPC timeouts (production configuration):**

| Parameter | Value | Notes |
|-----------|-------|-------|
| `HEARTBEAT_MS` | 75 | Leader heartbeat interval |
| `ELECTION_TIMEOUT_MIN_MS` | 600 | ~8× heartbeat (Raft recommends 5–10×) |
| `ELECTION_TIMEOUT_MAX_MS` | 1000 | Randomized upper bound |
| `RAFT_RPC_TIMEOUT_MS` | 150 | Individual AppendEntries/RequestVote timeout |
| `RAFT_PROPOSE_TIMEOUT_MS` | **5000** | Time leader waits for quorum commit on a single Propose |

---

## Slide 13: The Request Lifecycle in the Distributed System

```
1.  Agent sends AcquireGuard(agent_id, embedding, payload, operation_type) to dscc-proxy:50050
2.  Proxy resolves current Raft leader via GetLeader polling
3.  Proxy forwards AcquireGuard to leader node
4.  Leader validates inputs (agent_id non-empty, embedding non-empty)
5.  Leader checks IsLeader(); if not, returns FAILED_PRECONDITION + leader-address metadata
6.  Leader calls lock_table_.acquire(agent_id, embedding, θ) — may block in per-lock wait queue
7.  Once admitted, leader proposes ACQUIRE to Raft log, waits for quorum commit
8.  If quorum fails → release local lock, return UNAVAILABLE
9.  Leader performs Qdrant operation:
    - Write: upsert point with embedding + payload metadata (FNV-1a hash ID, 3 retries, 75ms backoff)
    - Read: vector similarity search query
10. If LOCK_HOLD_MS > 0 and operation is write: sleep (simulates extended critical section)
11. Leader proposes RELEASE to Raft log, waits for quorum + apply
12. Apply callback: lock_table_.apply_release(agent_id) → triggers rebalance_waiters_locked
13. Returns AcquireResponse with timing metadata and queue telemetry
```

---

## Slide 14: Leader-Aware Proxy

The proxy (`dscc-proxy`) is a C++ gRPC service that implements the same `LockService` interface as the nodes.

**Leader discovery:**
- Background thread polls `GetLeader` RPC on all backend nodes every `LEADER_POLL_MS` (100ms)
- Caches the current leader address; reuses gRPC channel cache per backend

**Request forwarding** (`ForwardWithLeaderRetry` template):
- Forwards AcquireGuard / ReleaseGuard / Ping to the current leader
- On failure (NOT_LEADER, UNAVAILABLE, DEADLINE_EXCEEDED):
  1. Extract `leader-address` from gRPC trailing metadata
  2. If redirect found, try the new leader
  3. Otherwise, call `RefreshLeader()` to re-poll all nodes
  4. Retry up to 3 times
- `REQUEST_TIMEOUT_MS` = 35000ms per forwarded RPC (accounts for lock_hold + Raft round-trips)

**Key design decision:** Proxy is written in C++ (same build), not Go (as originally planned in the architecture doc). This simplified the Docker build — one `Dockerfile` builds both `dscc-node` and `dscc-proxy` from the same CMake project.

---

## Slide 15: Architectural Caveat — Leader-Admitted-Then-Replicated

**The current admission path is:**

```
1. Leader: lock_table_.acquire()      ← locally admits request
2. Leader: raft_->Propose(ACQUIRE)    ← replicates to quorum
3. Leader: Qdrant operation
4. Leader: raft_->Propose(RELEASE)    ← replicates release to quorum
5. Apply callback: release lock, rebalance waiters
```

**Note:** Step 1 happens **before** step 2. The leader holds the lock locally before the ACQUIRE is durably committed through Raft.

**Why this matters:**
- If the leader crashes between steps 1 and 2, the lock was never replicated — it simply vanishes. The new leader's lock table doesn't know about it. No harm done (the Qdrant write also didn't happen).
- If the leader crashes between steps 2 and 3, the ACQUIRE is in the Raft log. The new leader's lock table holds this lock via apply callback, but the agent's RPC timed out and will retry. The retrying agent now conflicts with its own replicated lock — potential deadlock without TTL/lease.
- If the leader crashes between steps 3 and 4, the write is in Qdrant but RELEASE was never proposed. The lock is held indefinitely on the new leader's table until TTL/lease (not yet implemented).

**The fully correct approach** would be: replicate ACQUIRE through Raft first, apply, *then* admit locally. We chose the current ordering for simplicity and because the local-first path has lower latency on the hot path. Fixing this is a priority for the next phase.

---

## Slide 16: Challenges and Bugs — Lock Table Redesign

**Challenge 1: Global condition variable → per-lock queues**

The original `cv_.notify_all()` design worked for the single-node prototype but became a correctness and performance risk under concurrent multi-lock scenarios. The redesign required:

- Introducing `WaitQueueEntry` with per-waiter `shared_ptr<condition_variable>`
- Implementing `rebalance_waiters_locked()` — the most complex function in the lock table
- Handling the `granted` flag handoff: when a releaser grants a waiter directly during rebalance, the waiter must not re-run `find_conflict` (it was already checked under the same lock scope)
- Correctly propagating `queue_hops` across rebalance iterations to track fairness

**Subtle bug we fixed:** During rebalance, if waiter W is moved from lock A's queue to lock B's queue, and B is released while W's rebalance is still running, W could be notified on a stale CV. The fix: rebalance runs entirely under `mu_`, and notifications happen only after `mu_` is released.

---

## Slide 17: Challenges and Bugs — Raft Implementation

**Challenge 2: Custom Raft from scratch in C++**

We implemented Raft from the Ongaro & Ousterhout paper rather than using an existing library. Major issues encountered:

1. **Election livelock in 5-node cluster**: With the original heartbeat (50ms) and election timeout (150–300ms), the 5-node Docker cluster experienced frequent split votes and election cycling. Fix: increased to 75ms heartbeat, 600–1000ms election timeout (8–13× ratio instead of 3–6×).

2. **Log consistency on follower rejoin**: When a stopped follower restarts and receives `AppendEntries`, the `prev_log_index/prev_log_term` consistency check must correctly handle the follower's log being shorter than expected. We added conflict hints (`conflict_term`, `conflict_index`) in `AppendResponse` for faster rollback.

3. **Apply ordering**: The `ApplyLoop` runs on a dedicated thread. We had a race where `commit_index_` advanced but the apply thread hadn't yet called the callback. The `WaitUntilApplied()` method was added so that `LockServiceImpl` can block on RELEASE being applied before returning to the client.

4. **Leader address propagation**: `AppendRequest` needs `leader_service_address` (the client-facing port), not just `leader_id`. We added this field to the proto so followers can give meaningful redirect information to the proxy.

---

## Slide 18: Challenges — The RAFT_PROPOSE_TIMEOUT_MS Mystery

**Current value: 5000ms** (5 seconds)

This is the timeout for a single `Propose()` call — how long the leader waits for quorum to acknowledge a log entry.

**Why this is suspicious:**
- On a local Docker bridge network, AppendEntries round-trip should be 1–5ms
- With 5 nodes and quorum of 3, even one slow follower shouldn't matter
- The original plan specified 2000ms; we had to increase to 5000ms to stop seeing intermittent `UNAVAILABLE` errors

**Possible explanations we are investigating:**
- Container startup scheduling delays on Docker Desktop
- gRPC channel establishment overhead on first AppendEntries after election
- Lock table `apply_acquire` callback holding `mu_` during `on_commit_`, blocking the heartbeat thread if it tries to acquire the same mutex
- The replication path creates a new `AppendEntries` RPC per follower per entry; without pipelining, latency accumulates

**Status:** The system works reliably at 5000ms, but this is not within the range of healthy Raft behavior. This needs root-cause investigation.

---

## Slide 19: E2E Test Suite — 7 Scenarios

| # | Scenario | Agents | Tests | Validates |
|---|----------|--------|-------|-----------|
| 1 | Write A conflicts with Read B | A, B | A↔B conflict | Semantic conflict detection, read-write serialization, blocking metadata |
| 2 | Mixed write/read without forced conflicts | A, C, D | No conflicts | Distinct tasks complete in parallel (within max_spread threshold) |
| 3 | Write D conflicts with Read E | D, E | D↔E conflict | Second conflict pair, read-behind-write ordering |
| 4 | Full fan-in A through E | A, B, C, D, E | A↔B + D↔E | Mixed conflicts + parallelism for non-conflicting agents |
| 5 | **Kill leader during full fan-in** | A, B, C, D, E | Leader failover | Mid-scenario `docker compose stop` on leader; new election; agents complete |
| 6 | **Operate with one follower down** | A, B, D, E | Quorum resilience | Stop one follower; run scenario with 4 nodes; verify quorum still works |
| 7 | **Rejoined follower participates in quorum** | A, B | Catch-up + quorum | Restart stopped follower, stop a different one; verify the restarted node serves quorum |

**Validation per scenario:**
- All agents granted (gRPC status OK, `response.granted() == true`)
- Qdrant point count matches expected write count (reads don't produce points)
- Qdrant payload scroll contains correct `agent_id`, `source_file`, `raw_text`
- Conflict pairs: `finish_gap >= max(250, lock_hold_ms / 2)` ms apart — proves serialization
- Read operations in conflict pairs: `lock_wait_ms > 0`, `blocking_agent_id` is a write agent
- Distinct scenarios: completion spread `<= max(300, lock_hold_ms / 2)` ms — proves parallelism

---

## Slide 20: Curated Benchmark Suite — 10 Stress Scenarios

Beyond the E2E demo, we have a dedicated `dscc-benchmark` runner with 10 curated adversarial scenarios:

| # | Scenario | What it Tests |
|---|----------|---------------|
| 1 | The Thundering Herd | All agents fire identical writes simultaneously |
| 2 | The Semantic Interleaving | Alternating conflict/non-conflict agents |
| 3 | The Read-Starvation Trap | Reads stuck behind long writes on the same semantic region |
| 4 | The Permissive Sieve | Low-theta run where most operations pass through |
| 5 | The Strict Sieve | High-theta run where everything serializes |
| 6 | The Ghost Client | Agent disappears mid-scenario (simulates agent crash) |
| 7 | The Almost Collision | Near-threshold embeddings (synthetic vectors just below/above θ) |
| 8 | Queue Hopping | Agents that get requeued multiple times across lock chains |
| 9 | The Mixed Stagger | Staggered offsets with mixed read/write on overlapping regions |
| 10 | The 100% Read Stampede | All-read scenario testing read-read serialization behavior |

**Current status:** Some scenarios (Strict Sieve, Ghost Client, Almost Collision, Mixed Stagger) pass cleanly. Others (Thundering Herd, Semantic Interleaving, Read-Starvation Trap, Queue Hopping, 100% Read Stampede) still report correctness violations — overlap between conflict pairs in the recorded timestamps.

**Benchmark outputs:** Per-case timelines, live queue event stream (`[LOCK_QUEUE]`, `[LOCK_REQUEUE]`, `[LOCK_GRANT]`), raw JSON logs in `logs/benchmark_run_<timestamp>.json` with per-operation traces and explicit violation records.

---

## Slide 21: Workload Characteristics

**Write-dominant, low-to-moderate concurrency, high-latency critical sections:**

- Typical agent count per scenario: 2–10 agents
- Lock hold duration: 750ms default (simulates Qdrant write + extended critical section)
- Embedding dimension: 384 floats = 1,536 bytes per embedding per log entry
- Conflict detection is `O(n)` in active lock count (linear scan over `active_` map)
- Raft log entries per request: 2 (ACQUIRE + RELEASE) × embedding payload size
- Qdrant operations: 1 upsert or 1 vector search per request

**Read path is conservative:** Reads go through the same semantic locking path as writes. A read that conflicts with an active write waits. This is stricter than necessary — reads could potentially be allowed concurrent access — but it guarantees that a read always sees a consistent state (no reading while a conflicting write is in-flight).

**Network overhead:** Each ACQUIRE log entry replicates the full 384-float embedding to all followers. For a 5-node cluster, that's 4 × 1,536 bytes = ~6KB of network traffic per lock acquisition just for the embedding replication.

---

## Slide 22: CAP Theorem Analysis

In the context of the CAP theorem, our system makes the following tradeoffs:

**Consistency (C):** We prioritize this. Semantic serializability is the core correctness guarantee — conflicting operations must not overlap. The leader-only admission model ensures a single serialization point.

**Availability (A):** We sacrifice availability during leader election. When the leader goes down:
- The cluster is unavailable for write requests for the duration of leader election (~600–1000ms)
- The proxy retries and follows the new leader, but in-flight requests may time out
- With 5 nodes, we tolerate up to 2 node failures while maintaining quorum

**Partition tolerance (P):** Raft inherently handles network partitions:
- A partitioned leader cannot reach quorum → stops granting locks → refuses to make progress
- The majority partition elects a new leader and continues serving requests
- When the partition heals, the old leader steps down and catches up

**Our system is CP** — we favor consistency and partition tolerance over availability. This is the correct choice for a lock manager: **it is always better to refuse a request than to grant a conflicting lock.**

---

## Slide 23: Consistency Caveats in Practice

While we target CP, there are practical consistency gaps in the current implementation:

1. **Leader-admitted-then-replicated**: The local acquire happens before the Raft ACQUIRE is committed. If the leader crashes after local acquire but before Raft commit, the lock was "held" briefly without replication. No harm in practice (the Qdrant write also didn't happen), but this violates the strict CP guarantee.

2. **No durable lock snapshots**: Active lock state exists only in-memory across the Raft log replay. If all 5 nodes crash simultaneously, lock state is lost. (Raft log is in-memory; no WAL to disk.)

3. **No lease-based expiration**: If a lock is acquired and the agent disconnects, the lock can be held indefinitely until the RELEASE is committed. There is no TTL or heartbeat mechanism to automatically release abandoned locks.

4. **Timestamp-based correctness metric**: The benchmark detects overlap by comparing `[lock_acquired_unix_ms, lock_released_unix_ms)` intervals. Clock skew between the leader (where timestamps are recorded) and the benchmark runner could produce false violations.

---

## Slide 24: gRPC Proto Design and Telemetry

**Client-facing API** (`dscc.proto`):
```
service LockService {
  rpc Ping(PingRequest) returns (PingResponse);
  rpc AcquireGuard(AcquireRequest) returns (AcquireResponse);
  rpc ReleaseGuard(ReleaseRequest) returns (ReleaseResponse);
}
```

The `AcquireResponse` exposes rich telemetry for every request:

| Field | Purpose |
|-------|---------|
| `server_received_unix_ms` | When the leader received the RPC |
| `lock_acquired_unix_ms` | When the semantic lock was granted |
| `qdrant_write_complete_unix_ms` | When the Qdrant operation returned |
| `lock_released_unix_ms` | When the Raft RELEASE was applied |
| `lock_wait_ms` | Duration spent in the wait queue |
| `blocking_similarity_score` | Highest cosine similarity that caused blocking |
| `blocking_agent_id` | Which agent's lock caused the block |
| `wait_position` | Position in the wait queue when first blocked |
| `wake_count` | Number of times this request was woken from sleep |
| `queue_hops` | Number of times requeued behind a different lock |
| `active_lock_count` | Number of active locks at the time of grant |
| `serving_node_id` | Which node processed the request |
| `leader_redirect` | If not leader, the address of the current leader |

**Raft-internal API** (`dscc_raft.proto`): `RequestVote`, `AppendEntries`, `InstallSnapshot`, `GetLeader`

---

## Slide 25: Build and Deployment

**Build system:** CMake 3.16+, C++17 for core, C++20 for benchmarks (uses `std::barrier`)

| Target | Purpose |
|--------|---------|
| `dscc-node` | Distributed node binary |
| `dscc-proxy` | Leader-aware proxy binary |
| `dscc-e2e-bench` | End-to-end demo/test harness |
| `dscc-benchmark` | 10-scenario curated benchmark runner |
| `dscc-testbench` | In-process lock table unit tests |
| `dscc-raft-test` | In-process 3-node Raft test |

**Docker:** Single `Dockerfile` (Ubuntu 22.04 + build-essential + CMake + gRPC/protobuf dev packages) builds both `dscc-node` and `dscc-proxy`. The `docker-compose.yml` runs: Qdrant, Ollama, 5 dscc-nodes, 1 dscc-proxy.

**Dependency note:** Qdrant communication uses **raw TCP sockets with hand-crafted HTTP/1.1 requests** — no HTTP client library. This is known technical debt (3 retry attempts with 75ms×attempt backoff).

---

## Slide 26: What We Have Achieved

1. **Semantic serializability**: Agents with semantically similar payloads are correctly serialized; dissimilar agents proceed in parallel
2. **Distributed fault tolerance**: 5-node Raft cluster tolerates 2 simultaneous node failures
3. **Automatic leader election and failover**: Leader failure mid-scenario results in new leader election within ~1s; in-flight requests are retried by the proxy
4. **Read and write path**: Both operations go through the same semantic admission path
5. **Per-lock FIFO wait queues**: Eliminated thundering herd, improved fairness, enabled queue telemetry
6. **Rich observability**: Every request returns detailed timing, blocking, and queue metrics
7. **Comprehensive test coverage**: 7 E2E scenarios (including fault injection) + 10 curated benchmark scenarios + in-process unit tests + in-process Raft tests
8. **End-to-end automated orchestration**: The test harness manages Docker Compose lifecycle, service readiness, model pulling, collection management, and multi-threaded scenario execution

---

## Slide 27: What Remains — Towards Production Readiness

### Correctness and Reliability
- [ ] **Fix leader-admitted-then-replicated ordering** — replicate ACQUIRE through Raft first, then admit locally
- [ ] **Implement lease/TTL-based lock expiration** — auto-release locks after timeout to prevent indefinite holds from crashed agents
- [ ] **Durable Raft log** — write-ahead log to disk so lock state survives simultaneous cluster restart
- [ ] **Lock ownership enforcement** — only the lock holder (or the Raft apply callback) should be able to release a lock

### Performance and Scalability
- [ ] **Root-cause the 5000ms RAFT_PROPOSE_TIMEOUT_MS** — determine why quorum commitment takes longer than expected; profile gRPC channel setup, heartbeat/replication thread scheduling
- [ ] **Raft log entry batching** — currently each Propose is a single entry; batching reduces round-trips
- [ ] **Reduce embedding replication overhead** — consider storing embeddings by hash/reference rather than replicating full 384-float vectors in every log entry
- [ ] **Profile conflict detection** — `find_conflict_locked` is O(n) linear scan; consider spatial indexing (VP-trees, ball trees) for high active-lock counts
- [ ] **Review memory footprint** — each `SemanticLock` stores a 384-float centroid (1.5KB) + per-waiter queues with embedded copies of embeddings; memory audit needed

### Evaluation and Benchmarking
- [ ] **Physical agent testing** — replace synthetic benchmark scenarios with real LLM-driven agents issuing actual write/read workloads
- [ ] **Deployable image/service** — create a standalone Docker image or Helm chart that can be dropped in front of any Qdrant deployment
- [ ] **Performance evaluation** — measure throughput (ops/sec), latency percentiles (p50/p95/p99), under varying contention levels and cluster sizes
- [ ] **Ablation study on θ** — systematically vary theta from 0.30 to 0.95 and measure: conflict rate, serialization overhead, throughput, false positive/negative rates
- [ ] **Data structure optimization** — evaluate whether `unordered_map` is optimal; consider flat hash maps, lock-free structures, or sharded tables

### Infrastructure
- [ ] Replace raw-socket HTTP Qdrant client with a proper HTTP library
- [ ] Structured logging (replace `std::cout` with structured JSON or syslog)
- [ ] Prometheus/OpenTelemetry metrics export
- [ ] CI pipeline with automated test execution

---

## Slide 28: Ablation Study Plan — θ Sensitivity Analysis

We plan to systematically study how the theta (θ) threshold affects system behavior:

| θ Value | Expected Behavior | Metrics to Measure |
|---------|-------------------|-------------------|
| 0.30 | Very aggressive — most operations treated as conflicting | Throughput collapse, high serialization, near-zero parallelism |
| 0.50 | Moderate — loosely related texts conflict | Moderate serialization, some parallelism for truly distinct tasks |
| 0.65 | Balanced — only clearly related texts conflict | Good parallelism for diverse workloads, catches obvious semantic overlaps |
| 0.78 | Current default — strong paraphrases conflict | Our calibrated operating point for the demo workload |
| 0.85 | Conservative — only highly similar texts conflict | Higher parallelism, risk of missing weaker semantic overlaps |
| 0.95 | Very permissive — only near-identical texts conflict | Near-full parallelism, minimal protection against paraphrased conflicts |

**For each θ value, we will measure:**
- Conflict detection rate (% of request pairs flagged as conflicting)
- Average and max lock wait time
- Queue hop frequency
- Throughput (operations/second)
- Correctness: whether semantic violations occur at the vector DB level
- False negatives: semantically identical operations that slipped through due to high θ

---

## Slide 29: Scalability Concerns and Open Questions

1. **Vertical scalability of conflict detection**: Every incoming request scans all active locks. With 100+ concurrent agents, this becomes a bottleneck. Can we use approximate nearest-neighbor search (HNSW, VP-trees) on the active lock table itself?

2. **Raft log growth**: Each ACQUIRE entry carries a 384-float embedding. Without log compaction, the log grows unboundedly. We need log compaction with lock-table snapshots.

3. **Single-leader bottleneck**: All admission decisions go through the leader. This is a throughput ceiling. Could we partition the semantic space across multiple Raft groups (multi-Raft)?

4. **Qdrant as SPOF**: The lock manager is now HA, but Qdrant is still a single instance. A production deployment would need Qdrant clustering.

5. **Embedding model as SPOF**: Ollama is a single instance. Model serving failure blocks all requests that need fresh embeddings. Production: model serving cluster or pre-computed embeddings.

6. **Memory overhead of C++ implementation**: We are not expert C++ programmers. There are likely unnecessary copies, suboptimal container choices, and memory fragmentation issues. A thorough memory profiling pass is needed (Valgrind, AddressSanitizer, Heaptrack).

---

## Slide 30: Summary — Where We Are

| Aspect | Status |
|--------|--------|
| Single-node semantic locking | ✅ Complete, working |
| Per-lock wait queues with FIFO fairness | ✅ Complete, working |
| Raft consensus (custom C++ implementation) | ✅ Complete, working |
| 5-node cluster with automatic leader election | ✅ Complete, working |
| Leader-aware proxy with retry logic | ✅ Complete, working |
| Read + write paths through semantic admission | ✅ Complete, working |
| E2E test suite with fault injection (7 scenarios) | ✅ Complete, passing |
| Curated benchmark suite (10 stress scenarios) | ⚠️ Partial — some scenarios report violations |
| Leader-admitted-then-replicated fix | ❌ Not yet started |
| Lease/TTL lock expiration | ❌ Not yet started |
| Durable Raft log (WAL) | ❌ Not yet started |
| Performance evaluation and profiling | ❌ Not yet started |
| Real agent integration | ❌ Not yet started |
| Deployable production image | ❌ Not yet started |

**Bottom line:** We have a functioning distributed semantic lock manager with Raft replication, automatic failover, and comprehensive test infrastructure. The core correctness guarantee — semantic serializability — is achieved. The system is not yet production-grade: it needs the admission ordering fix, durable state, lease expiration, performance optimization, and evaluation with real agent workloads.

---

## Slide 31: Project Timeline and Next Steps

**Completed (Phase 1–4):**
- Single-node prototype with Ollama + Qdrant
- Lock table redesign (global CV → per-lock queues)
- Custom Raft implementation and integration
- 5-node cluster deployment
- Proxy implementation
- E2E and benchmark test suites

**Next immediate priorities:**
1. Root-cause and fix the RAFT_PROPOSE_TIMEOUT_MS issue
2. Fix leader-admitted-then-replicated ordering (Raft-first admission)
3. Implement lock lease/TTL expiration
4. Debug remaining benchmark violations

**Longer-term goals:**
5. Durable WAL for Raft log
6. Performance profiling and optimization (memory, CPU, network)
7. θ ablation study
8. Real agent integration testing
9. Deployable Docker image / service packaging
10. Production-grade observability (metrics, structured logging)
