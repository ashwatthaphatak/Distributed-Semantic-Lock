# DSCC (Single-Node Semantic Lock Manager)

## 1. Architecture

Flow:
`client -> gRPC LockService (AcquireGuard) -> ActiveLockTable -> Qdrant write -> lock release`

Core pieces:

- `src/lock_service_impl.cpp`
  - gRPC methods: `Ping`, `AcquireGuard`, `ReleaseGuard`
  - `AcquireGuard`: acquire semantic lock, write to Qdrant, release lock
- `src/active_lock_table.{h,cpp}`
  - in-memory active semantic locks
  - cosine similarity overlap check
  - condition-variable blocking
- `src/testbench.cpp`
  - deterministic concurrency testbench

Locking model:

- Each request has an embedding vector.
- If cosine similarity with an active lock is `>= theta`, request waits.
- Lock is released after write completes.

## 2. Run the Project

### Build

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

### Run server

```bash
./build/dscc-node
```

Useful environment variables:

- `PORT` (default: `50051`)
- `THETA` (default: `0.85`)
- `QDRANT_HOST` (default: `qdrant`)
- `QDRANT_PORT` (default: `6333`)
- `QDRANT_COLLECTION` (default: `dscc_memory`)

Run outside Docker with local Qdrant:

```bash
QDRANT_HOST=localhost ./build/dscc-node
```

### Run with Docker Compose (Qdrant + DSCC nodes)

```bash
docker compose up --build
```

## 3. Testing and How to Modify It

### Run testbench

```bash
./build/dscc-testbench
```

The testbench runs two scenarios with 5 concurrent agents:

- `Scenario-1`: independent embeddings -> concurrent lock ownership expected
- `Scenario-2`: near-identical embeddings -> serialized lock ownership expected

Printed output includes:

- scenario description
- agent embeddings (`agent-1`, `agent-2`, ...)
- event stream (`acquire-start`, `acquire-granted`, `released`)
- per-agent timing table
- scenario PASS/FAIL and final summary

### Modify test behavior

Edit `src/testbench.cpp`:

- `kThreads`: number of agents
- `kDim`: embedding dimension
- `kTheta`: conflict threshold used by the testbench
- `make_agent_id(...)`: naming format
- `no_conflict_embeddings` / `conflict_embeddings`: scenario vectors
- `sleep_for(std::chrono::milliseconds(200))`: simulated critical-section duration

Then rebuild and rerun:

```bash
cmake --build build -j"$(nproc)"
./build/dscc-testbench
```
