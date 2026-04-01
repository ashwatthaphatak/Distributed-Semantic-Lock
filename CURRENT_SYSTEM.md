# DSCC — Distributed Semantic Conflict Controller

> Historical note:
> this document reflects an earlier architecture snapshot and is no longer the
> authoritative description of the repository. For the current distributed
> runtime, benchmark flow, and repo layout, use `STATE.md`.

## Context for AI Models

This document is a deep, self-contained description of the DSCC project: its purpose, architecture, every source file, data flows, concurrency model, infrastructure, build system, and known limitations. Use this document to fully understand the system before making changes.

---

## 1. What This Project Is

DSCC is a **semantic lock manager** for multi-agent systems. When multiple AI agents attempt to write to a shared memory store (Qdrant vector database) concurrently, DSCC prevents **semantically conflicting** writes from executing simultaneously.

The core idea: before an agent's write is committed, its **embedding vector** is compared against all currently held locks using **cosine similarity**. If the similarity to any active lock meets or exceeds a configurable threshold **theta (θ)**, the incoming request **blocks** until the conflicting lock is released. If no conflict exists, the lock is granted immediately and the agent's write proceeds.

This is not a traditional mutex or row-level lock. The conflict domain is **semantic** — two requests conflict not because they touch the same key or row, but because their natural-language payloads are semantically similar according to their embedding vectors.

### Real-World Analogy

Imagine five clerks all trying to update a ledger. Two of them are working on "the Apple invoice for March" — they are doing semantically the same thing. A traditional lock would not catch this because they might be writing to different rows. DSCC recognizes that their *intent* overlaps and serializes them, while letting unrelated work (like "reboot the server") proceed in parallel.

---

## 2. Current Scope and Limitations

**What is implemented:**

- Single-node, in-memory semantic lock table
- gRPC server (`dscc-node`) that accepts lock acquisition requests carrying embedding vectors
- Cosine similarity conflict detection against all currently active locks
- Blocking wait (condition variable) when a conflict is detected
- Qdrant vector database integration for persisting agent writes after lock acquisition
- End-to-end benchmark harness that orchestrates Docker services, generates real embeddings via Ollama, runs multi-agent scenarios, and validates results
- In-process concurrency testbench for the lock table alone

**What is NOT implemented (future work from TODO.md §7):**

- Multi-node distributed coordination (no Raft, no Paxos, no leader election)
- Shared lock state across process boundaries
- Lease/heartbeat-based lock expiration
- Partition tolerance or recovery semantics
- Lock ownership enforcement on release (any caller can release any lock)
- Duplicate active lock prevention for the same agent_id
- Bounded waiting / acquisition timeouts

The word "Distributed" in the repository name is **aspirational**. The current system runs as a single gRPC server process with an in-memory lock table.

---

## 3. Repository Structure

```
Distributed-Semantic-Lock/
├── CMakeLists.txt                  # Build system (CMake 3.16+, C++17/C++20)
├── docker-compose.yml              # Orchestrates qdrant, ollama, dscc-node
├── docker/
│   └── Dockerfile                  # Ubuntu 22.04 image for dscc-node
├── proto/
│   └── dscc.proto                  # gRPC service contract (source of truth)
│   └── dscc.pb.cc                  # Checked-in generated protobuf (not used by CMake)
│   └── dscc.pb.h
│   └── dscc.grpc.pb.cc             # Checked-in generated gRPC stubs (not used by CMake)
│   └── dscc.grpc.pb.h
├── src/
│   ├── main.cpp                    # dscc-node entry point (gRPC server startup)
│   ├── lock_service_impl.h         # gRPC service class declaration
│   ├── lock_service_impl.cpp       # gRPC service implementation (lock + Qdrant write)
│   ├── active_lock_table.h         # Semantic lock table declaration
│   ├── active_lock_table.cpp       # Semantic lock table implementation (core algorithm)
│   ├── threadsafe_log.h            # Thread-safe line logger declaration
│   ├── threadsafe_log.cpp          # Thread-safe line logger implementation
│   ├── testbench.cpp               # In-process lock table concurrency test
│   └── e2e_bench.cpp               # Full end-to-end demo harness
├── demo_inputs/
│   ├── A.txt                       # "Process the Apple Inc invoice for March 2026..."
│   ├── B.txt                       # "Handle Apple invoice processing for March 2026..."
│   ├── C.txt                       # "Reboot the main application server in rack 3..."
│   ├── D.txt                       # "Generate the payroll summary for April 2026..."
│   └── E.txt                       # "Create the April 2026 payroll summary..."
├── README.md                       # User-facing build/run/output guide
├── TODO.md                         # Backlog of known issues and future work
├── LICENSE                         # MIT license
├── .gitignore
└── .dockerignore
```

---

## 4. Architecture Overview

### 4.1 System Components

The system has four runtime components during a full end-to-end run:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        e2e_bench (host)                            │
│                                                                     │
│  Reads demo_inputs/*.txt                                            │
│  Sends text to Ollama for embedding                                 │
│  Sends gRPC AcquireGuard to dscc-node                              │
│  Validates Qdrant writes after each scenario                        │
└──────┬──────────────────┬───────────────────────┬───────────────────┘
       │ HTTP POST        │ gRPC (proto3)          │ HTTP GET/POST
       │ /v1/embeddings   │ AcquireGuard           │ /collections/...
       ▼                  ▼                        ▼
┌──────────────┐   ┌──────────────────┐   ┌──────────────────┐
│   Ollama     │   │   dscc-node      │   │     Qdrant       │
│  (Docker)    │   │   (Docker)       │──▶│   (Docker)       │
│              │   │                  │   │                  │
│ Port 7997    │   │ Port 50051       │   │ Port 6333        │
│ (mapped from │   │                  │   │                  │
│  11434)      │   │ ActiveLockTable  │   │ Vector DB with   │
│              │   │ + Qdrant writer  │   │ cosine distance  │
└──────────────┘   └──────────────────┘   └──────────────────┘
```

**Ollama** — Embedding service. Runs `all-minilm:latest` model. Converts natural language text into 384-dimensional float vectors. Exposed on host port 7997 (mapped from container port 11434).

**dscc-node** — The semantic lock manager. A C++ gRPC server. Contains `LockServiceImpl` which owns an `ActiveLockTable` (in-memory). Receives embedding vectors via `AcquireGuard` RPC, checks for semantic conflicts, blocks or grants, writes to Qdrant while holding the lock, then releases. Exposed on port 50051.

**Qdrant** — Vector database. Receives upsert requests from dscc-node containing the agent's embedding vector and metadata payload (agent_id, source_file, raw_text, timestamp). Serves as the persistent memory store. Exposed on port 6333.

**e2e_bench** — The test harness. Runs on the host machine (not in Docker). Orchestrates the entire flow: starts Docker Compose stack, waits for service readiness, generates embeddings, launches concurrent gRPC requests, collects timing data, prints timelines, and validates Qdrant state.

### 4.2 Data Flow for a Single Agent Request

```
1. e2e_bench reads text from demo_inputs/X.txt
2. e2e_bench sends HTTP POST to Ollama /v1/embeddings with the text
3. Ollama returns a 384-float embedding vector
4. e2e_bench constructs an AcquireRequest protobuf message:
   - agent_id:            "scenario-<slug>-agent-X"
   - embedding:           [float × 384]
   - payload_text:        original text content
   - source_file:         "X.txt"
   - timestamp_unix_ms:   current time
5. e2e_bench sends gRPC AcquireGuard to dscc-node
6. dscc-node receives the request in LockServiceImpl::AcquireGuard
7. LockServiceImpl calls ActiveLockTable::acquire(agent_id, embedding, theta)
8. ActiveLockTable checks cosine similarity against every active lock:
   - If any similarity >= theta → block on condition_variable, re-check on wake
   - If no conflict → insert new SemanticLock into active_ vector, return
9. LockServiceImpl records lock_acquired timestamp
10. LockServiceImpl calls upsert_embedding_to_qdrant (raw HTTP PUT to Qdrant)
11. Qdrant stores the vector + payload metadata
12. LockServiceImpl records qdrant_write_complete timestamp
13. If LOCK_HOLD_MS > 0, sleep for that duration (simulates extended critical section)
14. LockServiceImpl calls ActiveLockTable::release(agent_id)
15. release removes the lock entry and calls cv_.notify_all()
16. Any blocked agents wake up and re-check overlap
17. LockServiceImpl records lock_released timestamp
18. AcquireResponse is returned to e2e_bench with all timing metadata
```

---

## 5. Core Algorithm: Semantic Lock Table

### 5.1 Data Structures

**File:** `src/active_lock_table.h`

```cpp
struct SemanticLock {
    std::string agent_id;
    std::vector<float> centroid;    // the embedding vector
    float threshold;                // theta at time of acquisition
};

struct AcquireTrace {
    bool waited = false;
    float blocking_similarity_score = 0.0f;
    std::string blocking_agent_id;
};

class ActiveLockTable {
    std::vector<SemanticLock> active_;   // currently held locks
    mutable std::mutex mu_;              // protects active_
    std::condition_variable cv_;         // wake blocked waiters on release
};
```

### 5.2 Acquire Algorithm

**File:** `src/active_lock_table.cpp`, function `ActiveLockTable::acquire`

```
ACQUIRE(agent_id, embedding, threshold):
  lock(mu_)
  LOOP:
    trace = overlap_trace(embedding, threshold)
    IF trace.waited == false:
      BREAK                          // no conflict, proceed
    ELSE:
      record blocking info
      log "[LOCK] agent_id blocked by blocking_agent_id"
      cv_.wait(lock)                 // release mutex, sleep until notified
      GO TO LOOP                     // re-check after waking
  
  active_.push_back(SemanticLock{agent_id, embedding, threshold})
  unlock(mu_)
  print_active_locks()
  RETURN trace
```

Key properties:
- The wait loop is a **standard condition variable pattern** — wake-and-recheck
- After waking, the agent re-scans ALL active locks, not just the one that released
- Multiple agents can hold locks simultaneously if their embeddings are dissimilar
- The lock is added to the active list only after confirming no conflict exists

### 5.3 Overlap Detection

**File:** `src/active_lock_table.cpp`, function `ActiveLockTable::overlap_trace`

For every entry in `active_`, compute cosine similarity between the incoming embedding and the entry's centroid. If any similarity >= threshold, report it. If multiple entries conflict, report the one with the highest similarity score.

### 5.4 Cosine Similarity

**File:** `src/active_lock_table.cpp`, function `ActiveLockTable::cosine_similarity`

Standard cosine similarity: `dot(a,b) / (||a|| * ||b||)`, computed in double precision, clamped to [-1, 1], returned as float. Returns 0.0 if vectors are empty or mismatched in dimension.

### 5.5 Release

**File:** `src/active_lock_table.cpp`, function `ActiveLockTable::release`

```
RELEASE(agent_id):
  lock(mu_)
  remove_if active_.agent_id == agent_id
  unlock(mu_)
  cv_.notify_all()        // wake ALL waiting agents
  print_active_locks()
```

`notify_all` is used (not `notify_one`) because releasing one lock may unblock multiple waiting agents whose conflicts were only with the released agent.

---

## 6. gRPC Service Layer

### 6.1 Proto Contract

**File:** `proto/dscc.proto`

```protobuf
service LockService {
  rpc Ping(PingRequest) returns (PingResponse);
  rpc AcquireGuard(AcquireRequest) returns (AcquireResponse);
  rpc ReleaseGuard(ReleaseRequest) returns (ReleaseResponse);
}
```

**AcquireRequest fields:**
| Field | Type | Purpose |
|---|---|---|
| `agent_id` | string | Unique identifier for the requesting agent |
| `embedding` | repeated float | Embedding vector to check for conflicts |
| `payload_text` | string | Raw text content to persist in Qdrant |
| `source_file` | string | Origin file name for metadata |
| `timestamp_unix_ms` | int64 | Client-side timestamp |

**AcquireResponse fields:**
| Field | Type | Purpose |
|---|---|---|
| `granted` | bool | Whether the lock was acquired and write succeeded |
| `message` | string | Human-readable status |
| `server_received_unix_ms` | int64 | When dscc-node received the RPC |
| `lock_acquired_unix_ms` | int64 | When the semantic lock was granted |
| `qdrant_write_complete_unix_ms` | int64 | When the Qdrant upsert returned |
| `lock_released_unix_ms` | int64 | When the semantic lock was released |
| `lock_wait_ms` | int64 | Duration spent waiting for the lock |
| `blocking_similarity_score` | float | Highest similarity that caused blocking |
| `blocking_agent_id` | string | Agent that caused the block |

### 6.2 AcquireGuard Implementation

**File:** `src/lock_service_impl.cpp`

The `AcquireGuard` RPC performs the following sequence atomically from the caller's perspective:

1. **Validate** — reject if agent_id or embedding is empty
2. **Acquire lock** — call `lock_table_.acquire(agent_id, embedding, theta_)` which may block
3. **Write to Qdrant** — call `upsert_embedding_to_qdrant(...)` using raw HTTP/1.1 over TCP sockets
4. **Hold** — if `lock_hold_ms_ > 0`, sleep (simulates extended critical section duration)
5. **Release lock** — call `lock_table_.release(agent_id)` and `cv_.notify_all()`
6. **Return** — populate AcquireResponse with all timing metadata

A `ScopeExit` guard ensures the lock is always released even if the Qdrant write fails.

### 6.3 Qdrant Communication

**File:** `src/lock_service_impl.cpp`

The server communicates with Qdrant using **raw TCP sockets and hand-crafted HTTP/1.1 requests** (no HTTP client library). This is a known technical debt item (TODO.md §3).

**Collection creation:** `PUT /collections/<name>` with `{"vectors":{"size":<dim>,"distance":"Cosine"}}`. Handles 200, 201, 409, and 400-already-exists as success.

**Point upsert:** `PUT /collections/<name>/points?wait=true` with a JSON body containing the point ID (FNV-1a hash of agent_id + timestamp), the embedding vector, and a metadata payload (agent_id, source_file, timestamp, raw_text). Retries up to 3 times with 75ms × attempt backoff on failure.

### 6.4 Point ID Generation

**File:** `src/lock_service_impl.cpp`, function `make_numeric_point_id`

Uses FNV-1a hash of the agent_id string, mixed with the timestamp shifted left by 22 bits, masked to a positive int64. This produces a deterministic numeric ID for Qdrant from string inputs.

### 6.5 Server Startup

**File:** `src/main.cpp`

Minimal entry point: reads `PORT` from environment (default 50051), creates a `LockServiceImpl`, registers it with a gRPC `ServerBuilder`, enables default health check service, starts listening, and calls `server->Wait()`.

---

## 7. Configuration

All configuration is via environment variables. There are two scopes: variables read by **dscc-node** (inside Docker) and variables read by **e2e_bench** (on the host).

### 7.1 dscc-node Environment Variables

| Variable | Default | Purpose |
|---|---|---|
| `PORT` | `50051` | gRPC listen port |
| `THETA` | `0.85` | Cosine similarity threshold for conflict detection |
| `LOCK_HOLD_MS` | `0` | Milliseconds to hold the lock after Qdrant write |
| `QDRANT_HOST` | `qdrant` | Hostname of the Qdrant instance |
| `QDRANT_PORT` | `6333` | Port of the Qdrant instance |
| `QDRANT_COLLECTION` | `dscc_memory` | Qdrant collection name |

### 7.2 e2e_bench / docker-compose Environment Variables

| Variable | Default | Purpose |
|---|---|---|
| `DSCC_THETA` | `0.78` | Passed to dscc-node container as `THETA` |
| `DSCC_LOCK_HOLD_MS` | `750` | Passed to dscc-node container as `LOCK_HOLD_MS` |
| `EMBEDDING_IMAGE` | `ollama/ollama:latest` | Docker image for the embedding service |
| `EMBEDDING_MODEL_ID` | `all-minilm:latest` | Ollama model to use for embeddings |
| `QDRANT_COLLECTION` | `dscc_memory_e2e` | Collection name for the e2e bench |
| `E2E_TEARDOWN` | unset | Set to `1` to auto-teardown Docker stack after run |

Note: `docker-compose.yml` sets `THETA=${DSCC_THETA:-0.78}` inside the dscc-node container, so the e2e bench's `DSCC_THETA` becomes the server's `THETA`. The server's internal default of `0.85` is only used if no environment variable is set at all.

---

## 8. Build System

### 8.1 CMake Targets

**File:** `CMakeLists.txt`

| Target | Sources | C++ Standard | Links | Purpose |
|---|---|---|---|---|
| `dscc_proto` (static lib) | Generated `dscc.pb.cc`, `dscc.grpc.pb.cc` | C++17 | protobuf, gRPC | Shared protobuf/gRPC stubs |
| `dscc-node` | `main.cpp`, `threadsafe_log.cpp`, `active_lock_table.cpp`, `lock_service_impl.cpp` | C++17 | `dscc_proto`, gRPC, protobuf, Threads | The gRPC server executable |
| `dscc-testbench` | `testbench.cpp`, `threadsafe_log.cpp`, `active_lock_table.cpp` | C++17 | Threads | In-process lock table test (no gRPC) |
| `dscc-e2e-bench` | `e2e_bench.cpp`, `threadsafe_log.cpp`, `lock_service_impl.cpp`, `active_lock_table.cpp` | **C++20** | `dscc_proto`, gRPC, protobuf, Threads | Full end-to-end demo harness |

### 8.2 Proto Code Generation

CMake runs `protoc` at build time to generate `dscc.pb.{cc,h}` and `dscc.grpc.pb.{cc,h}` into `${CMAKE_CURRENT_BINARY_DIR}/generated/`. The checked-in `proto/dscc.pb.*` and `proto/dscc.grpc.pb.*` files are **not** used by the build — they appear to be legacy artifacts.

### 8.3 Build Commands

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Or to build only the e2e bench:

```bash
cmake -S . -B /tmp/dslm_build
cmake --build /tmp/dslm_build --target dscc-e2e-bench -j$(nproc)
```

### 8.4 Dependencies

- **CMake** >= 3.16
- **C++17** (C++20 for `dscc-e2e-bench` due to `std::barrier`)
- **Protobuf** — found via CONFIG or MODULE mode
- **gRPC** — found via CONFIG (preferred) or pkg-config fallback
- **grpc_cpp_plugin** — required for gRPC stub generation
- **POSIX sockets** — used for raw HTTP to Qdrant and in the e2e bench
- **pthreads** — via `find_package(Threads)`

### 8.5 Docker Build

**File:** `docker/Dockerfile`

Ubuntu 22.04 base. Installs `build-essential`, `cmake`, `pkg-config`, `protobuf-compiler`, `protobuf-compiler-grpc`, `libprotobuf-dev`, `libgrpc++-dev`, `libgrpc-dev`. Copies the entire repo, runs `cmake -S . -B build && cmake --build build`, and sets `CMD ["./build/dscc-node"]`.

---

## 9. Docker Compose Stack

**File:** `docker-compose.yml`

Three services:

### 9.1 qdrant
- Image: `qdrant/qdrant`
- Port: `6333:6333`
- No host volume mount — data is ephemeral (lost when container is removed)

### 9.2 embedding-service
- Image: `ollama/ollama:latest` (overridable via `EMBEDDING_IMAGE`)
- Port: `7997:11434` (host 7997 → container 11434)
- Volume: `./.cache/ollama:/root/.ollama` (caches downloaded models between runs)

### 9.3 dscc-node
- Built from `docker/Dockerfile`
- Port: `50051:50051`
- Environment: `PORT=50051`, `THETA`, `LOCK_HOLD_MS`, `QDRANT_HOST=qdrant`, `QDRANT_PORT=6333`, `QDRANT_COLLECTION`
- Depends on: `qdrant`, `embedding-service`

---

## 10. End-to-End Benchmark Harness

**File:** `src/e2e_bench.cpp`

This is the primary demonstration and validation executable. It is a ~1240-line C++ program that runs on the host (not inside Docker) and exercises the full system.

### 10.1 Startup Sequence

1. Load configuration from environment variables
2. Print runtime configuration summary
3. Start Docker Compose stack (`docker compose up -d --build`)
4. Wait for Qdrant HTTP readiness (polls `/collections` or `/`)
5. Wait for Ollama embedding service readiness (polls `/api/tags`, `/v1/models`, `/`)
6. Detect if embedding container has failed (exited, dead, exec format error)
7. Pull the embedding model (`POST /api/pull` with `{"model":"all-minilm:latest","stream":false}`)
8. Wait for dscc-node gRPC readiness (sends `Ping` RPC in a loop)

### 10.2 Document Loading

Reads `demo_inputs/A.txt` through `E.txt`, sends each to Ollama's `/v1/embeddings` endpoint, and stores the resulting 384-dimension embedding vector alongside the original text.

### 10.3 Similarity Matrix

Computes and prints a pairwise cosine similarity matrix for all five documents. This shows which document pairs will conflict at the current theta.

Seeded demo intent:
- **A ↔ B**: Invoice processing conflict pair (~0.90 similarity)
- **D ↔ E**: Payroll summary conflict pair (~0.92 similarity)
- **C**: Distinct server reboot task (low similarity to all others)

### 10.4 Scenarios

Four predefined scenarios are run in sequence:

| # | Name | Agents | Expected Conflicts | Notes |
|---|---|---|---|---|
| 1 | Agents A and B describe the same invoice | A, B | A↔B | Tests basic two-agent semantic conflict |
| 2 | Agents A, C, and D run together | A, C, D | none | Tests distinct tasks run in parallel |
| 3 | Agents D and E describe the same payroll task | D, E | D↔E | Tests another conflict pair |
| 4 | Full fan-in with Agents A through E | A, B, C, D, E | A↔B, D↔E | Tests mixed conflicts and independence |

### 10.5 Scenario Execution

For each scenario:

1. **Reset** — delete the Qdrant collection, recreate it with correct vector dimensions
2. **Synchronize** — launch one thread per agent, synchronize all threads at a `std::barrier`
3. **Per-thread flow:**
   - Request fresh embedding from Ollama (even though embeddings were already cached — this measures embedding latency per scenario)
   - Construct `AcquireRequest` protobuf
   - Send `AcquireGuard` gRPC to dscc-node
   - Record all timing milestones (submit, embedding start/finish, DSLM enter/exit)
4. **Join** all threads
5. **Print timeline** — chronologically ordered events across all agents
6. **Print agent recaps** — per-agent summary of embedding time, wait time, blocking info, result
7. **Validate gRPC results** — all agents must be granted
8. **Validate Qdrant** — point count must match agent count, payload metadata must be present
9. **Validate conflict pairs** — conflict pairs must show serialization (finish times separated by at least `max(250, lock_hold_ms/2)` ms)
10. **Validate distinct scenarios** — non-conflicting agents must finish within a tight window (`max(300, lock_hold_ms/2)` ms)

### 10.6 Timeline Output

The timeline is the central output of the demo. It shows, in chronological order across all agents:

```
t+   0ms  Agent A submitted text
t+   0ms  Agent B submitted text
t+   1ms  Agent A entered embedding model
t+   1ms  Agent B entered embedding model
t+  45ms  Agent A left embedding model
t+  48ms  Agent B left embedding model
t+  49ms  Agent A entered DSLM
t+  50ms  Agent B entered DSLM
t+  52ms  Agent A reached DSLM server
t+  53ms  Agent B reached DSLM server
t+  54ms  Agent A acquired semantic lock
t+  55ms  Agent B blocked by Agent A (similarity 0.900 >= theta 0.780)
t+ 120ms  Agent A finished Qdrant write
t+ 870ms  Agent A released semantic lock
t+ 871ms  Agent B acquired semantic lock after waiting 818ms
t+ 935ms  Agent B finished Qdrant write
t+1686ms  Agent B released semantic lock
```

---

## 11. In-Process Testbench

**File:** `src/testbench.cpp`

A standalone test of `ActiveLockTable` without Docker, gRPC, Qdrant, or embeddings. Uses synthetic 8-dimensional vectors.

**Scenario 1 (no conflict):** 5 threads with orthogonal unit vectors (each has a single 1.0 in a unique dimension). Cosine similarity between any pair is 0. Expected: all agents run concurrently, max active > 1, overlapping hold intervals.

**Scenario 2 (full conflict):** 5 threads with nearly identical vectors (all 1.0 with tiny perturbations). Cosine similarity ≈ 1.0. Expected: only one agent active at a time, no overlapping hold intervals, strict serialization.

Each thread:
1. Waits at a start barrier (manual condition variable implementation)
2. Calls `acquire` (may block)
3. Holds the lock for 200ms
4. Calls `release`

Results report: peak active count, whether hold intervals overlapped, PASS/FAIL.

---

## 12. Thread-Safe Logging

**Files:** `src/threadsafe_log.h`, `src/threadsafe_log.cpp`

A single function `void log_line(const std::string& line)` that writes to stdout under a global mutex. Prevents interleaved output when multiple threads log concurrently. Used by the lock table, the gRPC server, the testbench, and the e2e bench.

---

## 13. Concurrency Model

### 13.1 Lock Table Concurrency

- **Mutex `mu_`** protects the `active_` vector. All reads and writes to `active_` are under this mutex.
- **Condition variable `cv_`** is used for the blocking wait pattern. Blocked agents sleep on `cv_` and are woken by `notify_all()` when any lock is released.
- The acquire loop follows the standard pattern: `while (conflict_exists) { cv_.wait(lock); }` — this handles spurious wakes and multiple conflicting locks.

### 13.2 gRPC Concurrency

gRPC uses its own thread pool to handle incoming RPCs. Each `AcquireGuard` call runs on its own thread. The `ActiveLockTable` handles the serialization internally via its mutex and condition variable. The `LockServiceImpl` instance (and its `lock_table_` member) is shared across all RPC threads.

### 13.3 E2E Bench Concurrency

The e2e bench uses `std::barrier` (C++20) to synchronize agent threads at the start of each scenario. All agents begin simultaneously, then proceed independently through embedding → gRPC → completion.

### 13.4 Potential Concurrency Issues (Known)

- No lock ownership enforcement: any thread can call `release` for any agent_id
- No duplicate lock prevention: the same agent_id can acquire multiple concurrent locks
- No timeout on the condition variable wait: a deadlocked or slow lock holder blocks others indefinitely
- `notify_all()` wakes all waiters even if only one can proceed (thundering herd)

---

## 14. Qdrant Integration Details

### 14.1 Collection Schema

Each Qdrant collection is created with:
- Vector size: determined by embedding dimension (384 for all-minilm)
- Distance metric: Cosine

### 14.2 Point Schema

Each upserted point contains:
- `id`: numeric int64 (FNV-1a hash-based)
- `vector`: the embedding float array
- `payload`:
  - `agent_id`: string
  - `source_file`: string
  - `raw_text`: string (the original text content)
  - `timestamp_unix_ms`: int64

### 14.3 Persistence

Qdrant runs without a host volume mount in the current docker-compose. Data is ephemeral — it is lost when the container is removed. The e2e bench also deletes and recreates the collection before each scenario.

---

## 15. Embedding Service Details

### 15.1 Ollama API

The e2e bench uses Ollama's OpenAI-compatible endpoint:
- `POST /v1/embeddings` with body `{"model":"all-minilm:latest","input":"<text>"}`
- Returns JSON with an `"embedding"` field containing a float array

Model pulling is done via Ollama's native endpoint:
- `POST /api/pull` with body `{"model":"all-minilm:latest","stream":false}`

Readiness is checked via:
- `GET /api/tags` (Ollama native)
- `GET /v1/models` (OpenAI-compatible)
- `GET /` (fallback)

### 15.2 Model

`all-minilm:latest` produces 384-dimensional embedding vectors. The model is cached in `.cache/ollama/` on the host via a Docker volume mount.

---

## 16. Demo Input Files

The five text files in `demo_inputs/` represent agent task descriptions:

| File | Content | Intent |
|---|---|---|
| A.txt | "Process the Apple Inc invoice for March 2026 and update accounts payable." | Invoice processing task |
| B.txt | "Handle Apple invoice processing for March 2026 and post it to accounts payable." | Same invoice, different wording |
| C.txt | "Reboot the main application server in rack 3 after the backup completes." | Unrelated server ops task |
| D.txt | "Generate the payroll summary for the April 2026 finance review." | Payroll task |
| E.txt | "Create the April 2026 payroll summary for finance review and compliance." | Same payroll, different wording |

**Conflict pairs:** A↔B (invoice), D↔E (payroll). **Distinct:** C (server reboot).

These files are editable. Changing them changes the semantic relationships and therefore the locking behavior.

---

## 17. Key Thresholds and Tuning

### 17.1 Theta (θ)

The cosine similarity threshold that determines whether two embeddings are "semantically conflicting."

| Theta | Behavior |
|---|---|
| 0.95 | Very permissive — only near-identical texts conflict |
| 0.78 | Moderate — strong paraphrases conflict (default for e2e bench) |
| 0.30 | Aggressive — even loosely related texts conflict |
| 0.10 | Very aggressive — almost everything conflicts |

### 17.2 Lock Hold Time

`LOCK_HOLD_MS` / `DSCC_LOCK_HOLD_MS` controls how long the server sleeps after a successful Qdrant write before releasing the lock. This simulates an extended critical section. Default is 750ms in the e2e bench, 0 in the server's internal default.

---

## 18. How to Run

### 18.1 Full End-to-End Demo

```bash
# Build
cmake -S . -B /tmp/dslm_build
cmake --build /tmp/dslm_build --target dscc-e2e-bench -j$(nproc)

# Run (leaves Docker stack up for inspection)
/tmp/dslm_build/dscc-e2e-bench

# Run with auto-teardown
E2E_TEARDOWN=1 /tmp/dslm_build/dscc-e2e-bench

# Run with custom theta
DSCC_THETA=0.30 /tmp/dslm_build/dscc-e2e-bench
```

### 18.2 In-Process Lock Table Test

```bash
cmake -S . -B /tmp/dslm_build
cmake --build /tmp/dslm_build --target dscc-testbench -j$(nproc)
/tmp/dslm_build/dscc-testbench
```

### 18.3 Manual Docker Stack

```bash
docker compose up -d --build qdrant embedding-service dscc-node
docker compose down
docker compose down -v   # also removes volumes
```

---

## 19. Layer Contract

### 19.1 What This Layer Does

- Accepts agent write requests carrying embedding vectors
- Detects semantic conflicts between concurrent requests using cosine similarity
- Serializes conflicting writes (blocks the later arrival until the earlier one completes)
- Persists the embedding + metadata to Qdrant
- Reports timing and blocking metadata back to the caller

### 19.2 What This Layer Does NOT Do

- No agent planning, orchestration, or LLM reasoning
- No embedding generation (the caller must supply the embedding)
- No distributed coordination across multiple dscc-node instances
- No persistent lock state (locks are in-memory, lost on restart)
- No authentication or authorization

### 19.3 Caller Expectations

- Supply a valid `agent_id`, `payload_text`, `source_file`, and `embedding`
- Embeddings must come from the same model and vector space to produce meaningful similarity scores
- Choose a theta that matches the desired conflict sensitivity
- Handle the `granted: false` response (currently only happens on Qdrant write failure)

### 19.4 Infrastructure Expectations

- Qdrant must be reachable over HTTP from dscc-node
- The embedding service must produce stable, fixed-dimension vectors
- Docker must be available for the e2e bench to orchestrate the stack

---

## 20. Known Technical Debt

From `TODO.md`, organized by category:

**Correctness:** No lock ownership enforcement, no duplicate lock prevention, no embedding dimension validation, no acquisition timeouts.

**API:** No structured error codes, no idempotency, AcquireGuard combines lock+write+release in one RPC with no acquire-only mode.

**Qdrant:** Uses raw socket HTTP instead of a proper client library, limited error parsing, basic retry logic.

**Testing:** No unit test framework (GTest/Catch2), no integration tests, no failure-path tests.

**Observability:** Ad-hoc stdout logging, no metrics, no structured log format.

**Deployment:** No health endpoint strategy, no CI pipeline, no startup config validation.

**Distributed (§7):** No shared state, no consensus protocol, no leases, no partition handling, no distributed fairness policy.

---

## 21. File-by-File Reference

| File | Lines | Role |
|---|---|---|
| `src/active_lock_table.h` | 47 | Declares `SemanticLock`, `AcquireTrace`, `ActiveLockTable` class |
| `src/active_lock_table.cpp` | 126 | Core algorithm: acquire (with blocking), release, cosine similarity |
| `src/lock_service_impl.h` | 54 | Declares `LockServiceImpl` gRPC service class |
| `src/lock_service_impl.cpp` | 448 | gRPC handlers, Qdrant HTTP communication, config loading |
| `src/main.cpp` | 27 | dscc-node entry point, gRPC server startup |
| `src/threadsafe_log.h` | 8 | Declares `log_line` |
| `src/threadsafe_log.cpp` | 17 | Implements `log_line` with global mutex |
| `src/testbench.cpp` | 281 | In-process ActiveLockTable concurrency test |
| `src/e2e_bench.cpp` | 1238 | Full end-to-end demo and validation harness |
| `proto/dscc.proto` | 49 | gRPC/protobuf service contract |
| `CMakeLists.txt` | 131 | Build system with 3 executables + proto library |
| `docker-compose.yml` | 34 | Docker stack: qdrant, ollama, dscc-node |
| `docker/Dockerfile` | 25 | Ubuntu 22.04 build image for dscc-node |
| `demo_inputs/A-E.txt` | 1 each | Agent task description payloads |
| `README.md` | 320 | User-facing documentation |
| `TODO.md` | 50 | Known issues and future work backlog |
