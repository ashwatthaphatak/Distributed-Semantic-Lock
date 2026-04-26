#!/usr/bin/env python3
"""
thundering_herd.py — standalone thundering-herd workload generator.

Mirrors the benchmark_runner's curated case #1 exactly:

  ScenarioKind::kThunderingHerd
    agent_count  : 10  (HERD_AGENTS)
    writes       : 10  (all writes)
    arrival_mode : kBurst  ← threading.Barrier releases all agents at t=0
    lock_hold_ms : 750     ← set on the server via LOCK_HOLD_MS env var

The C++ benchmark launches one thread per agent and calls
  std::this_thread::sleep_until(start + ms(0))
before each gRPC call, which collapses all submissions to the same instant.
This script does the identical thing with threading.Barrier(N): every agent
thread blocks on the barrier until the last one arrives, then all fire
simultaneously.

After each wave the script prints a result table and aggregate stats:

  ══ Wave 1 / 5 ═══════════════════════════════════════════════════
  Firing 10 agents simultaneously …

   #  Agent                        Finish    pos  hops  wake  lock_wait   total
  ──────────────────────────────────────────────────────────────────────────────
   1  sustainability_agent_1       +752 ms    0     0     1       0 ms    752 ms
   2  sustainability_agent_2       +1503 ms   1     0     2     751 ms   1503 ms
   …
  10  sustainability_agent_10      +7512 ms   9     3     4    6758 ms   7512 ms

  max queue depth : 9   max hops : 3   avg lock_wait : 3379 ms
  span            : 7512 ms            throughput    : 1.33 ops/s
  ══════════════════════════════════════════════════════════════════

────────────────────────────────────────────────────────────────────
Setup (one-time, on the friend's laptop)
────────────────────────────────────────────────────────────────────

  pip install grpcio grpcio-tools requests

  python -m grpc_tools.protoc \\
      -I proto \\
      --python_out=. \\
      --grpc_python_out=. \\
      proto/dscc.proto

────────────────────────────────────────────────────────────────────
Usage
────────────────────────────────────────────────────────────────────

  # Local (both on your machine):
  python thundering_herd.py

  # Remote via Tailscale (friend's laptop → your laptop):
  DSCC_PROXY=<YOUR_TS_IP>:50050 \\
  EMBEDDING_HOST=<YOUR_TS_IP> \\
  python thundering_herd.py

────────────────────────────────────────────────────────────────────
Environment variables
────────────────────────────────────────────────────────────────────

  DSCC_PROXY         gRPC proxy address          (default: localhost:50050)
  EMBEDDING_HOST     Embedding service host       (default: localhost)
  EMBEDDING_PORT     Embedding service port       (default: 7997)
  EMBEDDING_MODEL    Model name                   (default: all-minilm:latest)
  HERD_AGENTS        Agents per burst             (default: 10)
  HERD_WAVES         Number of waves  0=infinite  (default: 5)
  HERD_WAVE_GAP_S    Seconds between waves        (default: 3.0)
  HERD_TIMEOUT_S     gRPC call timeout            (default: 60)
"""

import json
import os
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Optional

import requests

# ── Configuration ─────────────────────────────────────────────────────────────

PROXY_TARGET    = os.environ.get("DSCC_PROXY",         "localhost:50050")
EMBEDDING_HOST  = os.environ.get("EMBEDDING_HOST",     "localhost")
EMBEDDING_PORT  = os.environ.get("EMBEDDING_PORT",     "7997")
EMBEDDING_MODEL = os.environ.get("EMBEDDING_MODEL",    "all-minilm:latest")
HERD_AGENTS     = int(os.environ.get("HERD_AGENTS",    "10"))
HERD_WAVES      = int(os.environ.get("HERD_WAVES",     "5"))
HERD_WAVE_GAP_S = float(os.environ.get("HERD_WAVE_GAP_S", "3.0"))
HERD_TIMEOUT_S  = int(os.environ.get("HERD_TIMEOUT_S",    "60"))

# ── Proto stubs ────────────────────────────────────────────────────────────────

try:
    import dscc_pb2
    import dscc_pb2_grpc
except ImportError:
    sys.exit(
        "ERROR: proto stubs not found.\n"
        "Generate them once with:\n"
        "  python -m grpc_tools.protoc -I proto "
        "--python_out=. --grpc_python_out=. proto/dscc.proto"
    )

import grpc

# ── Embedding ─────────────────────────────────────────────────────────────────

def fetch_embedding(text: str) -> list[float]:
    url = f"http://{EMBEDDING_HOST}:{EMBEDDING_PORT}/v1/embeddings"
    try:
        resp = requests.post(
            url,
            json={"model": EMBEDDING_MODEL, "input": text},
            timeout=30,
        )
        resp.raise_for_status()
    except requests.RequestException as e:
        sys.exit(
            f"ERROR: could not reach embedding service at {url}\n"
            f"  {e}\n"
            f"If running via Tailscale, set EMBEDDING_HOST=<server-ts-ip>"
        )
    return resp.json()["data"][0]["embedding"]


def load_concept_a_payload() -> str:
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "demo_inputs", "A.json")
    if not os.path.exists(path):
        sys.exit(
            f"ERROR: {path} not found.\n"
            "Clone the full repository so demo_inputs/ is present."
        )
    with open(path) as f:
        data = json.load(f)
    schedule = data.get("payload_schedule", [])
    return schedule[0]["payload"] if schedule else data["payload"]

# ── Per-agent result ───────────────────────────────────────────────────────────

@dataclass
class AgentResult:
    agent_id:        str
    ordinal:         int
    submit_ms:       float = 0.0
    finish_ms:       float = 0.0
    elapsed_ms:      float = 0.0
    granted:         bool  = False
    wait_position:   int   = 0
    queue_hops:      int   = 0
    wake_count:      int   = 0
    lock_wait_ms:    int   = 0
    active_locks:    int   = 0
    blocking_agent:  str   = ""
    error:           Optional[str] = None

# ── The burst ─────────────────────────────────────────────────────────────────

def run_agent(
    ordinal:     int,
    barrier:     threading.Barrier,
    channel:     grpc.Channel,
    payload_text: str,
    embedding:   list[float],
    results:     list,
    wave_base_ns: list,        # single-element list so the thread can read it
) -> None:
    agent_id = f"sustainability_agent_{ordinal}-{uuid.uuid4().hex[:4]}"
    result = AgentResult(agent_id=agent_id, ordinal=ordinal)
    results[ordinal] = result

    stub = dscc_pb2_grpc.LockServiceStub(channel)

    request = dscc_pb2.AcquireRequest(
        agent_id=agent_id,
        embedding=embedding,
        payload_text=payload_text,
        source_file="A.json",
        timestamp_unix_ms=int(time.time() * 1000),
        operation_type=dscc_pb2.AcquireRequest.OPERATION_TYPE_WRITE,
    )

    # ── Barrier: every agent blocks here until ALL are ready, then they
    #    all release simultaneously — equivalent to sleep_until(start + ms(0))
    #    in the C++ benchmark runner.
    barrier.wait()

    t0 = time.perf_counter_ns()
    result.submit_ms = (t0 - wave_base_ns[0]) / 1e6

    try:
        resp = stub.AcquireGuard(request, timeout=HERD_TIMEOUT_S)
        t1 = time.perf_counter_ns()
        result.finish_ms    = (t1 - wave_base_ns[0]) / 1e6
        result.elapsed_ms   = (t1 - t0) / 1e6
        result.granted       = resp.granted
        result.wait_position = resp.wait_position
        result.queue_hops    = resp.queue_hops
        result.wake_count    = resp.wake_count
        result.lock_wait_ms  = resp.lock_wait_ms
        result.active_locks  = resp.active_lock_count
        result.blocking_agent = resp.blocking_agent_id
    except grpc.RpcError as e:
        t1 = time.perf_counter_ns()
        result.finish_ms  = (t1 - wave_base_ns[0]) / 1e6
        result.elapsed_ms = (t1 - t0) / 1e6
        result.error = f"{e.code()}: {e.details()}"


def fire_burst(
    wave_num:     int,
    total_waves:  int,
    channel:      grpc.Channel,
    payload_text: str,
    embedding:    list[float],
) -> list[AgentResult]:
    """Launch HERD_AGENTS threads, barrier-sync them, fire, join, return results."""
    n       = HERD_AGENTS
    barrier = threading.Barrier(n)
    results = [None] * n
    wave_base_ns = [0]  # set just before threads hit the barrier

    threads = []
    for i in range(n):
        t = threading.Thread(
            target=run_agent,
            args=(i, barrier, channel, payload_text, embedding, results, wave_base_ns),
            daemon=True,
        )
        threads.append(t)

    label = f"{wave_num}/{total_waves}" if total_waves > 0 else f"{wave_num}/∞"
    _banner(f"Wave {label}")
    print(f"  Spawning {n} agents — all will fire at the same instant …\n")

    # Record the wave start just before threads launch so finish_ms is
    # relative to when the wave began, matching the benchmark's first_submit.
    wave_base_ns[0] = time.perf_counter_ns()

    for t in threads:
        t.start()
    for t in threads:
        t.join()

    return results  # type: ignore[return-value]

# ── Display ───────────────────────────────────────────────────────────────────

_W = 76  # table width

def _banner(title: str) -> None:
    print(f"\n  ══ {title} {'═' * (_W - len(title) - 5)}")

def _divider() -> None:
    print("  " + "─" * _W)

def print_wave_results(results: list[AgentResult]) -> None:
    # Header
    print(f"  {'#':>3}  {'Agent':<32}  {'Finish':>8}  {'pos':>4}  {'hops':>4}  "
          f"{'wake':>4}  {'lock_wait':>10}  {'total':>8}")
    _divider()

    max_pos   = 0
    max_hops  = 0
    sum_wait  = 0
    first_finish = float("inf")
    last_finish  = 0.0
    errors    = 0

    for r in sorted(results, key=lambda x: x.finish_ms):
        if r is None:
            continue
        if r.error:
            errors += 1
            print(f"  {'!':>3}  {r.agent_id:<32}  {'ERROR':>8}  "
                  f"{'—':>4}  {'—':>4}  {'—':>4}  {'—':>10}  "
                  f"  {r.error}")
            continue

        status = "✓" if r.granted else "✗"
        hop_str  = str(r.queue_hops)  if r.queue_hops  else "—"
        wait_str = f"{r.lock_wait_ms} ms" if r.lock_wait_ms else "—"

        print(f"  {status}{r.ordinal + 1:>2}  {r.agent_id:<32}  "
              f"+{r.finish_ms:>6.0f}ms  "
              f"{r.wait_position:>4}  {hop_str:>4}  {r.wake_count:>4}  "
              f"{wait_str:>10}  {r.elapsed_ms:>7.0f}ms")

        max_pos   = max(max_pos,  r.wait_position)
        max_hops  = max(max_hops, r.queue_hops)
        sum_wait += r.lock_wait_ms
        first_finish = min(first_finish, r.finish_ms)
        last_finish  = max(last_finish,  r.finish_ms)

    _divider()

    good = [r for r in results if r is not None and not r.error]
    n    = len(good)
    span_s     = last_finish / 1000.0 if last_finish else 0
    throughput = n / span_s if span_s > 0 else 0
    avg_wait   = sum_wait / n if n else 0

    print(f"\n  max queue depth : {max_pos:<4}   max hops : {max_hops}")
    print(f"  avg lock_wait   : {avg_wait:.0f} ms   span : {last_finish:.0f} ms"
          f"   throughput : {throughput:.2f} ops/s")
    if errors:
        print(f"  ⚠  {errors} RPC error(s) — check server logs")
    print(f"  {'═' * _W}\n")

# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    print("━" * (_W + 2))
    print("  Thundering-Herd Workload Generator")
    print(f"  proxy={PROXY_TARGET}  agents={HERD_AGENTS}"
          f"  waves={'∞' if HERD_WAVES == 0 else HERD_WAVES}"
          f"  gap={HERD_WAVE_GAP_S}s")
    print("━" * (_W + 2))

    print(f"\n  Loading A.json payload …")
    payload_text = load_concept_a_payload()
    print(f"  Payload  : {payload_text[:80]}…")

    print(f"  Embedding: fetching from {EMBEDDING_HOST}:{EMBEDDING_PORT} …")
    embedding = fetch_embedding(payload_text)
    print(f"  Embedding: {len(embedding)}-dim vector ready")
    print(f"\n  All {HERD_AGENTS} agents will use the SAME embedding → "
          f"every pair conflicts (cosine ≫ θ).\n")

    channel = grpc.insecure_channel(PROXY_TARGET)

    wave = 0
    try:
        while True:
            wave += 1
            results = fire_burst(wave, HERD_WAVES, channel, payload_text, embedding)
            print_wave_results(results)

            if HERD_WAVES > 0 and wave >= HERD_WAVES:
                break

            print(f"  Waiting {HERD_WAVE_GAP_S}s for queue to drain before next wave …")
            time.sleep(HERD_WAVE_GAP_S)

    except KeyboardInterrupt:
        print("\n  Interrupted.")
    finally:
        channel.close()

    print("  Done.")


if __name__ == "__main__":
    main()
