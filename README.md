# DSCC / DSLM

Distributed semantic lock manager in front of Qdrant.

The system accepts natural-language payloads plus embedding vectors and blocks requests whose cosine similarity to an active semantic lock is greater than or equal to a configured threshold `theta`.

## Architecture

Current end-to-end request path:

```text
demo_inputs/*.json
    -> embedding-service
    -> dscc-proxy
    -> current dscc-node leader
    -> ActiveLockTable
    -> Raft ACQUIRE replication
    -> Qdrant read/write
    -> Raft RELEASE replication
```

Main runtime pieces:

- `dscc-node`
  - lock service + Raft service on each node
- `dscc-proxy`
  - leader-aware forwarding layer for client traffic
- `embedding-service`
  - embedding generation for demo and benchmark inputs
- `qdrant`
  - vector storage and vector search

Main source files:

- [src/active_lock_table.h](/home/ubuntu/cpp_projects/DSLM/src/active_lock_table.h)
- [src/active_lock_table.cpp](/home/ubuntu/cpp_projects/DSLM/src/active_lock_table.cpp)
- [src/lock_service_impl.h](/home/ubuntu/cpp_projects/DSLM/src/lock_service_impl.h)
- [src/lock_service_impl.cpp](/home/ubuntu/cpp_projects/DSLM/src/lock_service_impl.cpp)
- [src/proxy_service_impl.h](/home/ubuntu/cpp_projects/DSLM/src/proxy_service_impl.h)
- [src/proxy_service_impl.cpp](/home/ubuntu/cpp_projects/DSLM/src/proxy_service_impl.cpp)
- [src/raft_node.h](/home/ubuntu/cpp_projects/DSLM/src/raft_node.h)
- [src/raft_node.cpp](/home/ubuntu/cpp_projects/DSLM/src/raft_node.cpp)
- [src/e2e_bench.cpp](/home/ubuntu/cpp_projects/DSLM/src/e2e_bench.cpp)
- [src/benchmark_runner.cpp](/home/ubuntu/cpp_projects/DSLM/src/benchmark_runner.cpp)

For a detailed current-state description, see [STATE.md](/home/ubuntu/cpp_projects/DSLM/STATE.md).

## Build

Configure once:

```bash
cmake -S . -B /tmp/dslm_build
```

Build the main targets:

```bash
cmake --build /tmp/dslm_build --target dscc-node -j"$(nproc)"
cmake --build /tmp/dslm_build --target dscc-proxy -j"$(nproc)"
cmake --build /tmp/dslm_build --target dscc-e2e-bench -j"$(nproc)"
cmake --build /tmp/dslm_build --target dscc-benchmark -j"$(nproc)"
```

Useful extra targets:

```bash
cmake --build /tmp/dslm_build --target dscc-testbench -j"$(nproc)"
cmake --build /tmp/dslm_build --target dscc-raft-test -j"$(nproc)"
```

## Demo Inputs

Current seeded inputs live in:

- [demo_inputs/A.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/A.json)
- [demo_inputs/B.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/B.json)
- [demo_inputs/C.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/C.json)
- [demo_inputs/D.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/D.json)
- [demo_inputs/E.json](/home/ubuntu/cpp_projects/DSLM/demo_inputs/E.json)

Each file supports:

- `payload`
- `scheduled_offset_ms`
- `operation`
- optional `payload_schedule`

When `payload_schedule` is present, both the e2e harness and the curated benchmark consume the first scheduled entry for that agent/template.

## End-to-End Demo

Run the full demo harness:

```bash
/tmp/dslm_build/dscc-e2e-bench
```

Run with automatic teardown:

```bash
E2E_TEARDOWN=1 /tmp/dslm_build/dscc-e2e-bench
```

Useful environment variables:

- `DSCC_THETA`
- `DSCC_LOCK_HOLD_MS`
- `EMBEDDING_IMAGE`
- `EMBEDDING_MODEL_ID`
- `QDRANT_COLLECTION`
- `E2E_TEARDOWN`

Examples:

```bash
DSCC_THETA=0.55 /tmp/dslm_build/dscc-e2e-bench
DSCC_THETA=0.90 DSCC_LOCK_HOLD_MS=1000 /tmp/dslm_build/dscc-e2e-bench
```

## Curated Benchmark Runner

The benchmark runner executes 10 hard-coded scenarios instead of a matrix sweep.

Run it with teardown:

```bash
E2E_TEARDOWN=1 /tmp/dslm_build/dscc-benchmark
```

The benchmark now provides:

- live queue-event output
- per-case timeline output
- explicit correctness-violation markers on the timeline
- raw JSON written to `logs/benchmark_run_<timestamp>.json`

Current scenario set:

1. The Thundering Herd
2. The Semantic Interleaving
3. The Read-Starvation Trap
4. The Permissive Sieve
5. The Strict Sieve
6. The Ghost Client
7. The Almost Collision
8. Queue Hopping
9. The Mixed Stagger
10. The 100% Read Stampede

Useful benchmark environment variables:

- `DSLM_BENCH_OUTPUT`
  - explicit JSON output path
- `E2E_TEARDOWN`
  - automatically stop the stack when the run exits

## Reading the Benchmark Output

### Live queue events

During a case you will now see lines such as:

```text
[LOCK_QUEUE] agent=sustainability_agent_4 waiting_on=sustainability_agent_1 similarity=1.000 queue_position=3 theta=0.550
[LOCK_REQUEUE] agent=sustainability_agent_4 waiting_on=sustainability_agent_2 similarity=1.000 queue_position=2 queue_hops=1 theta=0.550
[LOCK_GRANT] agent=sustainability_agent_4 queue_hops=1 active_locks=1
```

These come directly from the active lock table and show why a request entered or re-entered a queue.

### Timeline

The per-case timeline shows:

- when a request blocked
- who blocked it
- the similarity that caused the block
- when it was granted
- queue hops
- when it released
- explicit correctness-violation events when two conflicting lock intervals overlap

### Correctness

Benchmark correctness means:

- if two requests have pairwise similarity `>= theta`, they should not overlap in the active critical section

A violation means:

- two conflicting requests were both active at the same time according to the recorded lock-acquire and lock-release timestamps

## Docker

Start the stack manually:

```bash
docker compose up -d --build qdrant embedding-service dscc-node-1 dscc-node-2 dscc-node-3 dscc-node-4 dscc-node-5 dscc-proxy
```

Stop it:

```bash
docker compose down
```

## Notes

- CMake generates protobuf/gRPC code in the build directory.
- Generated protobuf files should stay out of the source tree.
- `STATE.md` is the authoritative current-state document for this repository.
