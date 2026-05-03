# Setup and Installation

**Authors: Ashwattha Phatak, Ayush Gala**
CSC 724 — Advanced Distributed Systems, NC State University

---

## Prerequisites

### System requirements
- Linux or macOS (tested on Ubuntu 22.04 and macOS 14+)
- Docker ≥ 24.0 and Docker Compose v2
- CMake ≥ 3.16
- C++17-capable compiler (GCC ≥ 11 or Clang ≥ 14)
- gRPC and Protobuf (≥ 3.21)
- Python ≥ 3.9 (for plotting scripts)

### Install dependencies — macOS (Homebrew)
```bash
brew install cmake grpc protobuf
```

### Install dependencies — Ubuntu
```bash
sudo apt-get update
sudo apt-get install -y cmake build-essential libgrpc++-dev \
    libprotobuf-dev protobuf-compiler-grpc
```

### Install Python plotting dependencies
```bash
pip install matplotlib numpy pandas
```

---

## Build

### Configure (once)
```bash
cmake -S . -B /tmp/dslm_build
```

### Build core binaries
```bash
# The DSCC node (runs as one of 5 cluster members)
cmake --build /tmp/dslm_build --target dscc-node -j"$(nproc)"

# The leader-aware proxy (client-facing entry point)
cmake --build /tmp/dslm_build --target dscc-proxy -j"$(nproc)"
```

### Build test and benchmark targets
```bash
# End-to-end integration harness
cmake --build /tmp/dslm_build --target dscc-e2e-bench -j"$(nproc)"

# Curated 13-scenario benchmark runner
cmake --build /tmp/dslm_build --target dscc-benchmark -j"$(nproc)"

# In-process ActiveLockTable unit tests (no Docker required)
cmake --build /tmp/dslm_build --target dscc-testbench -j"$(nproc)"

# In-process Raft regression tests (no Docker required)
cmake --build /tmp/dslm_build --target dscc-raft-test -j"$(nproc)"
```

> CMake generates protobuf/gRPC stubs into the build directory. Do not commit generated `.pb.cc`/`.pb.h` files.

---

## Running the stack

### Start the full Docker Compose stack
```bash
docker compose up -d --build \
    qdrant embedding-service \
    dscc-node-1 dscc-node-2 dscc-node-3 dscc-node-4 dscc-node-5 \
    dscc-proxy
```

This starts:
- 1 Qdrant vector database (port 6333)
- 1 Ollama embedding service (host port 7997)
- 5 DSCC nodes (ports 50051–50055 for LockService, 50061–50065 for Raft)
- 1 DSCC proxy (port 50050)

> **Port conflict on macOS:** If port 50051 is already in use (e.g. reserved by `launchd`), change the first number in the `dscc-node-1` port mapping in `docker-compose.yml` from `"50051:50051"` to an available port such as `"50056:50051"`. Internal Raft communication uses Docker's internal network and is unaffected.

### Verify services are up
```bash
# Check Qdrant
curl -s http://localhost:6333/collections | jq .

# Check embedding service
curl -s http://localhost:7997/v1/models | jq .

# Ping a DSCC node via grpcurl
grpcurl -plaintext localhost:50051 dscc.LockService/Ping
```

### Stop the stack
```bash
docker compose down
```

---

## Running tests

### In-process ActiveLockTable tests (no Docker needed)
```bash
/tmp/dslm_build/dscc-testbench
```
28 scenarios covering FIFO ordering, parallel reads, conflict serialization, and waiter rebalancing.

### In-process Raft tests (no Docker needed)
```bash
/tmp/dslm_build/dscc-raft-test
```
14 scenarios covering happy path, leader crash, follower catch-up, split vote, and semantic conflict under Raft.

### End-to-end integration harness (requires running stack)
```bash
/tmp/dslm_build/dscc-e2e-bench
# With automatic teardown:
E2E_TEARDOWN=1 /tmp/dslm_build/dscc-e2e-bench
```

---

## Running benchmarks

See [README.md](README.md) for the full benchmark documentation covering single, matrix, and soak modes.

---

## Environment variables reference

| Variable | Default | Used by |
|---|---|---|
| `DSCC_THETA` | `0.75` | e2e-bench, benchmark |
| `DSCC_LOCK_HOLD_MS` | `750` | e2e-bench, benchmark |
| `EMBEDDING_MODEL_ID` | `qwen3-embedding:0.6b` | e2e-bench, benchmark |
| `QDRANT_COLLECTION` | `dslm_test` | e2e-bench, benchmark |
| `E2E_TEARDOWN` | `0` | e2e-bench, benchmark |
| `DSLM_RUN_MODE` | `single` | benchmark (single/matrix/soak) |
| `DSLM_MATRIX_PROFILE` | all | benchmark matrix mode |
| `DSLM_MATRIX_THETA` | all | benchmark matrix mode |
| `DSLM_SOAK_DURATION_MIN` | `120` | benchmark soak mode |
| `PROXY_PORT` | `50050` | dscc-proxy |
| `BACKEND_NODES` | see compose | dscc-proxy |
