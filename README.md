# DSCC (Distributed Semantic Concurrency Control)

Semantic lock manager / guard service prototype for coordinating concurrent writes to a vector database.

Authors:
- Ashwattha Phatak
- Ayush Gala

## A. Project Overview

DSCC is intended to act as a distributed semantic concurrency control guard in front of vector DB writes. In the current repository state, the implemented vertical slice is a C++ gRPC server with a `LockService` API (`Ping`, `AcquireGuard`, `ReleaseGuard`) and stubbed responses for guard acquisition/release. A Docker Compose setup also starts Qdrant and multiple DSCC nodes, but DSCC is not yet wired to Qdrant or any distributed coordination logic.

In-scope today (implemented in repo):
- gRPC service contract for lock/guard operations (`proto/dscc.proto`)
- C++ gRPC server executable (`dscc-node`)
- Docker build/compose scaffolding for multiple DSCC nodes + Qdrant

Out-of-scope today (not implemented yet):
- Real lock manager semantics / conflict detection
- Distributed coordination between DSCC nodes
- Actual vector DB write interception/integration (Qdrant is present in compose only)
- Automated tests / benchmarks / persistence / observability beyond stdout logs

## B. Current Status (What’s Done vs Not Done)

### DONE
- [x] gRPC/protobuf contract defined for `LockService` with `Ping`, `AcquireGuard`, `ReleaseGuard` (`proto/dscc.proto`)
- [x] Generated protobuf/gRPC C++ sources checked into `proto/`
- [x] `dscc-node` C++ server scaffold implemented and builds with CMake (`CMakeLists.txt`)
- [x] Server binds to configurable `PORT` env var (default `50051`) in `src/main.cpp`
- [x] `Ping` RPC implemented as a simple echo-style health check (`pong to <from_node>`)
- [x] `AcquireGuard` RPC stub returns `granted=true` and message `"stub: granted"`
- [x] `ReleaseGuard` RPC stub returns `success=true`
- [x] Dockerfile builds and runs the C++ server on Ubuntu 22.04 base
- [x] `docker-compose.yml` defines Qdrant + 3 DSCC node containers

### TODO
- [ ] Implement actual `LockManager` / semantic conflict checks (currently no locking state is stored)
- [ ] Connect DSCC guard decisions to vector DB operations (Qdrant service is not used by server code)
- [ ] Add distributed coordination / replication / consensus (no inter-node communication code exists)
- [ ] Add request validation and richer error handling (RPC stubs always succeed)
- [ ] Add tests (unit/integration) and benchmark harnesses
- [ ] Add config wiring (`src/node_config.h` exists but is not used by `main.cpp`)

## C. Architecture (As Implemented Today)

```text
grpcurl / client
      |
      v
+---------------------------+
| DSCC Node (`dscc-node`)   |
| - gRPC server             |
| - LockServiceImpl         |
| - stub guard responses    |
+---------------------------+
      |
      v
[Future LockManager / state]
      |
      v
[Future vector DB integration (Qdrant)]
```

### Layer Responsibilities

Client (`grpcurl`, app client):
- Sends gRPC requests to `dscc.LockService`
- Can test RPCs directly with `-proto proto/dscc.proto` (reflection not required)

DSCC Node (implemented today):
- Starts a gRPC server and registers `LockServiceImpl` (`src/main.cpp`)
- Handles RPC requests with stub logic (`src/lock_service_impl.cpp`)
- Logs server bind address to stdout

Future LockManager / vector DB integration (not implemented):
- Lock state, queues, conflict detection, release semantics
- Forwarding/guarding actual vector DB writes
- Inter-node coordination and consistency

## D. Repository Map (Code Walkthrough)

### Top-level files

`CMakeLists.txt`
- Builds `dscc-node` from `src/*.cpp` plus checked-in generated protobuf sources in `proto/`
- Finds `Protobuf` and `gRPC` via system packages
- Links `gRPC::grpc++`, `gRPC::grpc++_reflection`, and `protobuf::libprotobuf`
- Important: CMake does **not** generate protobuf code; it compiles the generated files already in `proto/`

`docker-compose.yml`
- Starts one Qdrant container (`6333`) and three DSCC nodes (`5001`, `5002`, `5003`)
- Sets `NODE_ID` and `PORT` env vars for each DSCC node
- Current role: deployment scaffold / local demo topology (no DSCC↔Qdrant integration in code yet)

`README.md`
- Project documentation (this file)

### `proto/`

`proto/dscc.proto`
- Source-of-truth gRPC contract for the DSCC API
- Defines:
  - `Ping(PingRequest) -> PingResponse`
  - `AcquireGuard(AcquireRequest) -> AcquireResponse`
  - `ReleaseGuard(ReleaseRequest) -> ReleaseResponse`
- `AcquireRequest` currently contains `tx_id` and `embedding` (repeated float)

`proto/dscc.pb.h`, `proto/dscc.pb.cc`
- Generated protobuf message code from `dscc.proto`
- Do not edit by hand; regenerate when proto changes

`proto/dscc.grpc.pb.h`, `proto/dscc.grpc.pb.cc`
- Generated gRPC service stubs/base classes from `dscc.proto`
- `LockServiceImpl` derives from the generated service base

### `src/`

`src/main.cpp`
- Entry point for `dscc-node`
- Reads `PORT` env var (default `50051`)
- Enables gRPC health check + reflection plugin
- Registers `LockServiceImpl` and blocks on `server->Wait()`

`src/lock_service_impl.h`
- Declares the gRPC service implementation class
- Overrides `Ping`, `AcquireGuard`, `ReleaseGuard`

`src/lock_service_impl.cpp`
- Current request handlers
- `Ping`: returns `"pong to <from_node>"`
- `AcquireGuard`: always returns granted (stub)
- `ReleaseGuard`: always returns success (stub)

`src/node_config.h`
- `NodeConfig` struct reading `NODE_ID` / `PORT` from environment
- Currently not referenced by the running server path (`main.cpp` uses `PORT` directly)
- Note: header default port is `5001`, but `main.cpp` default is `50051`; runtime behavior follows `main.cpp`

### `docker/`

`docker/Dockerfile`
- Ubuntu 22.04-based image
- Installs build dependencies (`cmake`, protobuf, gRPC dev packages, compiler toolchain)
- Copies repo, builds `dscc-node`, and runs `./build/dscc-node`

### `build/` (generated locally)

`build/`
- CMake build output directory (generated)
- Not source of truth; safe to recreate

### Start Here (New Contributor)

Read these first, in order:
1. `proto/dscc.proto` (API surface and request/response types)
2. `src/lock_service_impl.cpp` (current behavior and stubs)
3. `src/main.cpp` (server boot + runtime config)
4. `CMakeLists.txt` (build assumptions and linked deps)
5. `docker-compose.yml` (intended multi-node local topology)

## E. Build + Run (Ubuntu 22)

### Prerequisites (Ubuntu 22.04, ARM64/x86_64)

Install system packages used by the current CMake build:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  protobuf-compiler \
  libprotobuf-dev \
  libgrpc++-dev \
  libgrpc-dev \
  grpc-proto \
  protobuf-compiler-grpc
```

Notes:
- The repo compiles on Ubuntu 22 with system `gRPC`/`protobuf` packages.
- `protobuf-compiler-grpc` provides `grpc_cpp_plugin` needed for manual regeneration.
- Generated protobuf/grpc C++ files are checked into `proto/`, so regeneration is only needed when `proto/dscc.proto` changes.

### Generate protobuf/gRPC C++ code (only when proto changes)

```bash
protoc -I proto \
  --cpp_out=proto \
  --grpc_out=proto \
  --plugin=protoc-gen-grpc="$(which grpc_cpp_plugin)" \
  proto/dscc.proto
```

Expected updated files:
- `proto/dscc.pb.h`
- `proto/dscc.pb.cc`
- `proto/dscc.grpc.pb.h`
- `proto/dscc.grpc.pb.cc`

### Build with CMake

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

Binary output:
- `build/dscc-node`

### Run the server

Default port (`50051`):

```bash
./build/dscc-node
```

Custom port:

```bash
PORT=5001 ./build/dscc-node
```

What to expect on stdout:
- `Server listening on 0.0.0.0:<PORT>`

### Test RPCs with `grpcurl` (without reflection)

Install `grpcurl` (one option):

```bash
sudo apt-get install -y grpcurl
```

If `grpcurl` is not in your apt repo on Ubuntu 22, install from release binary or Go toolchain, then run:

Ping:

```bash
grpcurl -plaintext \
  -proto proto/dscc.proto \
  -d '{"from_node":"cli"}' \
  localhost:50051 \
  dscc.LockService/Ping
```

AcquireGuard (stubbed, always granted today):

```bash
grpcurl -plaintext \
  -proto proto/dscc.proto \
  -d '{"tx_id":"tx-1","embedding":[0.1,0.2,0.3]}' \
  localhost:50051 \
  dscc.LockService/AcquireGuard
```

ReleaseGuard (stubbed, always success today):

```bash
grpcurl -plaintext \
  -proto proto/dscc.proto \
  -d '{"tx_id":"tx-1"}' \
  localhost:50051 \
  dscc.LockService/ReleaseGuard
```

Expected current behavior:
- `Ping` returns a `"pong to ..."` message
- `AcquireGuard` returns `{"granted": true, "message": "stub: granted"}`
- `ReleaseGuard` returns `{"success": true}`

### Docker Compose (local topology scaffold)

Bring up Qdrant + three DSCC nodes:

```bash
docker compose up --build
```

Ports:
- Qdrant: `6333`
- DSCC node 1: `5001`
- DSCC node 2: `5002`
- DSCC node 3: `5003`

Important current limitation:
- Qdrant is started by Compose, but DSCC server code does not yet connect to or call Qdrant.

## F. Development Workflow

Typical iteration loop:
1. Edit `proto/dscc.proto` (API) or `src/*.cpp` (server behavior)
2. If proto changed, regenerate generated files in `proto/`
3. Rebuild with CMake
4. Run `./build/dscc-node` (set `PORT` if needed)
5. Test RPCs using `grpcurl -proto proto/dscc.proto ...`

Logs / runtime output:
- Server logs are printed to stdout/stderr
- gRPC startup/bind errors will also appear on stderr

Environment variables used today:
- `PORT`: used by `src/main.cpp` to choose listening port (default `50051`)
- `NODE_ID`: set in Docker Compose, but not currently used by `main.cpp` / service logic

## G. Milestones & Timeline (From Proposal)

The project proposal/timeline was not included in the request (the prompt contains a placeholder: `<PASTE THE FULL PROJECT PROPOSAL DOCUMENT HERE, INCLUDING TIMELINE>`). This section is intentionally left as a tracked template so it can be filled in accurately once the proposal is pasted.

### Timeline Import Status

- [ ] Proposal timeline pasted into issue/chat
- [ ] Milestones converted into repo task checklist
- [ ] File/module mapping added for each milestone
- [ ] Deliverables/test/demo commands added per milestone

### Milestone Template (fill from proposal)

- [ ] **Milestone 1: <proposal title> (<date range>)**
  - Done looks like: `<acceptance criteria>`
  - Likely files/modules: `proto/...`, `src/...`, `CMakeLists.txt`, `docker-compose.yml`
  - Deliverables: `<tests / benchmark / demo commands>`

- [ ] **Milestone 2: <proposal title> (<date range>)**
  - Done looks like: `<acceptance criteria>`
  - Likely files/modules: `src/...`
  - Deliverables: `<tests / benchmark / demo commands>`

- [ ] **Milestone 3: <proposal title> (<date range>)**
  - Done looks like: `<acceptance criteria>`
  - Likely files/modules: `src/...`, `proto/...`
  - Deliverables: `<tests / benchmark / demo commands>`

### Suggested Next Engineering Work (until timeline is pasted)

- [ ] Implement in-memory lock state / guard table behind `AcquireGuard` and `ReleaseGuard`
- [ ] Add conflict decision logic using request embeddings (even basic placeholder heuristic is better than unconditional grant)
- [ ] Wire `NODE_ID` / `NodeConfig` into `main.cpp` and service logging
- [ ] Add unit tests for service methods and an integration test using `grpcurl` or a small client
- [ ] Decide how DSCC will interface with Qdrant (proxy, sidecar, or client-library integration)

## H. How to Contribute (Teammate Onboarding)

### If You’re New, Do This First

1. Install dependencies (Ubuntu 22 packages listed above)
2. Build the project:
   ```bash
   cmake -S . -B build
   cmake --build build -j"$(nproc)"
   ```
3. Run the server:
   ```bash
   ./build/dscc-node
   ```
4. In another shell, run `grpcurl` smoke tests (use the commands in Section E)

### Where to Implement Next (based on current repo state)

Priority areas:
- `src/lock_service_impl.cpp`: replace stubbed grant/release responses with real stateful logic
- `src/` (new files likely needed): lock manager, queues, conflict checks, in-memory state
- `proto/dscc.proto`: expand API only if required by the lock manager/distributed protocol
- `src/main.cpp` + config handling: wire `NodeConfig`, structured logging, runtime options
- Future distributed coordination modules (not present yet): inter-node RPC/client code, replication/consensus

### Contribution Notes

- Treat `proto/dscc.proto` as the API source of truth
- Do not hand-edit generated files in `proto/`; regenerate them
- Keep README/status docs aligned with what is actually implemented (especially stubs vs real logic)
