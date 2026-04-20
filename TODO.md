# TODO

## 1. Correctness

- [ ] Enforce lock ownership on release (only lock owner can release).
- [x] Prevent duplicate active locks for the same `agent_id`. *(apply_acquire_locked and insert_pending_locked overwrite if agent_id already present)*
- [ ] Validate embedding dimensions across requests before lock acquisition.
- [ ] Decide and enforce semantic overlap rule (`incoming threshold` vs `per-lock threshold`).
- [ ] Add bounded waiting / timeout support for blocked acquisitions.

## 2. API and Service Behavior

- [ ] Add explicit error codes in gRPC responses (invalid input, timeout, write failure).
- [ ] Add request idempotency behavior for retries.
- [ ] Add an explicit "acquire-only" mode vs current "acquire + write + release" RPC behavior.

## 3. Qdrant Integration

- [ ] Replace raw socket HTTP with a maintained HTTP client library.
- [ ] Parse HTTP response body and surface detailed write errors.
- [x] Add retry policy with backoff for transient Qdrant failures. *(upsert_embedding_to_qdrant retries up to 3 times with 75ms × attempt backoff)*
- [ ] Add startup health check for Qdrant availability.

## 4. Testing

- [x] Add unit tests for `ActiveLockTable` overlap and wait behavior. *(testbench.cpp — 5 concurrency scenarios)*
- [x] Add integration tests for gRPC `AcquireGuard`/`ReleaseGuard`. *(raft_test.cpp — 7 suites; e2e_bench.cpp — full-stack scenarios; benchmark_runner.cpp — 13 curated cases)*
- [ ] Add failure-path tests (Qdrant down, bad vector size, invalid payloads).
- [ ] Add deterministic tests for timeout and retry behavior.

## 5. Observability

- [ ] Replace ad-hoc `std::cout` with structured logging format.
- [x] Add metrics: active lock count, wait time, acquisition latency, Qdrant write latency. *(AcquireResponse exposes lock_wait_ms, active_lock_count, queue_hops, wake_count, blocking_similarity_score; benchmark_runner emits per-case and matrix CSV metrics)*
- [x] Add lock contention counters and conflict-rate reporting. *(LOCK_QUEUE / LOCK_REQUEUE / LOCK_GRANT log events with similarity, queue position, hops; benchmark_runner computes blocked_rate, serialization_score, conflicting_overlap_violations)*

## 6. Deployment

- [x] Add a simple health endpoint/readiness strategy for `dscc-node`. *(grpc::EnableDefaultHealthCheckService(true) in both main.cpp and proxy_main.cpp)*
- [ ] Add CI build + test pipeline.
- [x] Add runtime configuration validation at startup (port, theta, qdrant host/port). *(read_int_from_env / read_float_from_env with min/max bounds; proxy checks for empty BACKEND_NODES)*

## 7. Distributed SLM

- [x] Shared active lock state across nodes. *(Raft-replicated ACQUIRE/RELEASE log entries; apply callback syncs ActiveLockTable on all replicas)*
- [x] Node-to-node coordination protocol (leader/follower or consensus). *(Custom Raft over gRPC — leader election, log replication, commit/apply sequencing via dscc_raft.proto)*
- [ ] Lease/heartbeat handling for lock expiration. *(Raft heartbeats exist for leader liveness, but no lease-based expiry for abandoned locks)*
- [x] Partition handling and recovery semantics. *(Raft quorum prevents isolated leader from committing; follower catch-up via AppendEntries on rejoin)*
- [x] Distributed contention control and fairness policy. *(Per-lock FIFO waiter queues with requeue and hop tracking; leader-only admission via wait_for_admission)*
