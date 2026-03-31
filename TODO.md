# TODO

## 1. Correctness

- [ ] Enforce lock ownership on release (only lock owner can release).
- [ ] Prevent duplicate active locks for the same `agent_id`.
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
- [ ] Add retry policy with backoff for transient Qdrant failures.
- [ ] Add startup health check for Qdrant availability.

## 4. Testing

- [ ] Add unit tests for `ActiveLockTable` overlap and wait behavior.
- [ ] Add integration tests for gRPC `AcquireGuard`/`ReleaseGuard`.
- [ ] Add failure-path tests (Qdrant down, bad vector size, invalid payloads).
- [ ] Add deterministic tests for timeout and retry behavior.

## 5. Observability

- [ ] Replace ad-hoc `std::cout` with structured logging format.
- [ ] Add metrics: active lock count, wait time, acquisition latency, Qdrant write latency.
- [ ] Add lock contention counters and conflict-rate reporting.

## 6. Deployment

- [ ] Add a simple health endpoint/readiness strategy for `dscc-node`.
- [ ] Add CI build + test pipeline.
- [ ] Add runtime configuration validation at startup (port, theta, qdrant host/port).

## 7. Distributed SLM (Not Implemented Yet)

- [ ] Shared active lock state across nodes.
- [ ] Node-to-node coordination protocol (leader/follower or consensus).
- [ ] Lease/heartbeat handling for lock expiration.
- [ ] Partition handling and recovery semantics.
- [ ] Distributed contention control and fairness policy.
