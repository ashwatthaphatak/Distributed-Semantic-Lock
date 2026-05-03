# proto — Protobuf / gRPC Definitions

**Author: Ashwattha Phatak**  
CSC 724 — Advanced Distributed Systems, NC State University

---

## Files

### `dscc.proto` — client-facing LockService API

Defines the RPC interface that clients (e2e harness, benchmark runner, proxy) use to acquire and release semantic locks.

Key messages:
- `AcquireRequest` — agent ID, natural-language payload, pre-computed embedding vector, Qdrant operation metadata, and the similarity threshold θ
- `AcquireResponse` — granted lock ID and timing fields
- `ReleaseRequest / ReleaseResponse` — lock ID to release
- `PingRequest / PingResponse` — liveness probe

### `dscc_raft.proto` — internal Raft RPC messages

Defines the RPC interface used between DSCC nodes for consensus replication.

Key messages:
- `RequestVoteRequest / RequestVoteResponse` — leader election
- `AppendEntriesRequest / AppendEntriesResponse` — log replication and heartbeats; carries `LogEntry` items (ACQUIRE / RELEASE commands)
- `InstallSnapshotRequest / InstallSnapshotResponse` — snapshot transfer for lagging followers
- `GetLeaderRequest / GetLeaderResponse` — leader discovery used by the proxy poll loop

---

## Generated code

CMake generates `.pb.cc`, `.pb.h`, `.grpc.pb.cc`, and `.grpc.pb.h` files into the build directory (`/tmp/dslm_build/`) at configure time. These generated files are excluded from the repository via `.gitignore`.

To regenerate manually:

```bash
cmake -S . -B /tmp/dslm_build
# generated stubs appear under /tmp/dslm_build/
```
