"""
Thundering-herd workload generator for the Distributed Semantic Lock Manager.

Mirrors the benchmark_runner's curated case #1 — "The Thundering Herd":

  ScenarioKind::kThunderingHerd
    agent_count  : 10
    writes       : 10  (all writes, zero reads)
    theta        : 0.55
    lock_hold_ms : 750
    arrival_mode : kBurst   ← all requests fired simultaneously at t=0
    arrival_gap  : 0

Every Locust user sends the *same* payload (A.json's first entry — the
sustainability massing-concept text) so ALL users conflict with each other
(cosine similarity ≫ theta).  Combined with zero startup stagger this forces
the server's semantic-lock queue to grow as deep as the user count and causes
visible queue-hopping as positions shift between wake cycles.

The four thundering-herd signals exposed as custom Locust request events:

    ThunderingHerd/queue_hops        — how many times this request re-queued
    ThunderingHerd/wait_position     — deepest queue position observed
    ThunderingHerd/lock_wait_ms      — total wall-clock time blocked in queue
    ThunderingHerd/active_lock_count — server-side active lock count at release

These appear as separate "request types" in the Locust Web UI and --csv output
so you can watch queues grow and agents hop in real time.

───────────────────────────────────────────────────────────────
Remote / Tailscale setup (friend's laptop → your laptop)
───────────────────────────────────────────────────────────────

On YOUR laptop (the server):

    docker compose up -d --build     # starts proxy on :50050, embeddings on :7997

On your FRIEND'S laptop (the locust client):

    # 1. Clone the repo (needed for demo_inputs/ and proto/)
    git clone <repo-url>
    cd Distributed-Semantic-Lock

    # 2. Install Python dependencies
    pip install locust grpcio grpcio-tools requests

    # 3. Generate gRPC stubs (one-time)
    python -m grpc_tools.protoc \\
        -I proto \\
        --python_out=. \\
        --grpc_python_out=. \\
        proto/dscc.proto

    # 4. Run the herd (replace <YOUR_TS_IP> with your Tailscale IP)
    DSCC_PROXY=<YOUR_TS_IP>:50050 \\
    EMBEDDING_HOST=<YOUR_TS_IP> \\
    locust -f locustfile_thundering_herd.py

    # Then open http://localhost:8089, set users=10, spawn rate=10, and start.
    # Or headless (10 users, full burst spawn, 3-minute run):
    DSCC_PROXY=<YOUR_TS_IP>:50050 \\
    EMBEDDING_HOST=<YOUR_TS_IP> \\
    locust -f locustfile_thundering_herd.py --headless -u 10 -r 10 --run-time 3m

───────────────────────────────────────────────────────────────
Environment variables
───────────────────────────────────────────────────────────────

    DSCC_PROXY                  gRPC proxy address
                                (default: localhost:50050)
                                (tailscale: <server-ts-ip>:50050)

    EMBEDDING_HOST              Embedding service host
                                (default: localhost)
                                (tailscale: <server-ts-ip>)

    EMBEDDING_PORT              Embedding service port
                                (default: 7997)

    EMBEDDING_MODEL             Model name
                                (default: all-minilm:latest)

    DSCC_HERD_OP_INTERVAL_MS    Target interval between successive ops
                                per user (default: 200 ms — much tighter
                                than the regular locustfile's 1000 ms so
                                the queue never drains between rounds)

    DSCC_HERD_OP_JITTER_MS      +/- jitter around the interval
                                (default: 50 ms)

    DSCC_HERD_WAVE_PERIOD_S     When > 0, each user fires ONE burst
                                immediately then sleeps for this many
                                seconds before the next burst, producing
                                repeating queue spikes you can watch drain.
                                Set to 0 (default) for continuous pressure.

    DSCC_HERD_LOG_HOPS          1 = print a one-liner per request showing
                                queue_hops, wait_position, lock_wait_ms
                                (default: 1)
"""

import glob
import json
import logging
import os
import random
import threading
import time
import uuid

import grpc
import requests
from locust import User, task, events

logger = logging.getLogger(__name__)

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
DEMO_INPUTS_DIR = os.path.join(PROJECT_ROOT, "demo_inputs")

# ── Connection targets ──────────────────────────────────────────────────────
PROXY_TARGET = os.environ.get("DSCC_PROXY", "localhost:50050")
EMBEDDING_HOST = os.environ.get("EMBEDDING_HOST", "localhost")
EMBEDDING_PORT = os.environ.get("EMBEDDING_PORT", "7997")
EMBEDDING_MODEL = os.environ.get("EMBEDDING_MODEL", "all-minilm:latest")

# ── Thundering-herd workload controls ───────────────────────────────────────
# Tight interval to keep the queue perpetually long.  The benchmark itself
# uses burst arrival (all at t=0); continuous fire at 200 ms achieves a
# similar steady-state effect without running the server dry between bursts.
HERD_OP_INTERVAL_MS = int(os.environ.get("DSCC_HERD_OP_INTERVAL_MS", "200"))
HERD_OP_JITTER_MS   = int(os.environ.get("DSCC_HERD_OP_JITTER_MS",   "50"))

# Wave mode: 0 = continuous fire; N > 0 = burst then sleep N seconds.
WAVE_PERIOD_S = float(os.environ.get("DSCC_HERD_WAVE_PERIOD_S", "0"))

# When truthy, print a per-request line showing queue metrics.
LOG_HOPS = os.environ.get("DSCC_HERD_LOG_HOPS", "1") == "1"

# ── Module-level shared state ───────────────────────────────────────────────
# All users share a single pre-computed embedding for A.json's first payload.
# This maximises the probability that every in-flight request conflicts with
# every other, which is exactly what the benchmark's kThunderingHerd does.
_HERD_PAYLOAD_TEXT: str = ""
_HERD_EMBEDDING: list = []
_init_lock = threading.Lock()
_initialised = False

# Atomic counter for the sustainability_agent_N naming convention used in the
# benchmark runner (next_agent_id(role_counters, "sustainability_agent")).
_agent_counter = 0
_agent_counter_lock = threading.Lock()


def _next_agent_id() -> str:
    global _agent_counter
    with _agent_counter_lock:
        n = _agent_counter
        _agent_counter += 1
    return f"sustainability_agent_{n}"


# ── Initialisation ──────────────────────────────────────────────────────────

def _load_concept_a_payload() -> str:
    """Return the first payload text from demo_inputs/A.json."""
    path = os.path.join(DEMO_INPUTS_DIR, "A.json")
    if not os.path.exists(path):
        # Fallback: search for any file whose first entry mentions "massing"
        for p in sorted(glob.glob(os.path.join(DEMO_INPUTS_DIR, "*.json"))):
            with open(p) as f:
                data = json.load(f)
            schedule = data.get("payload_schedule", [])
            text = schedule[0]["payload"] if schedule else data.get("payload", "")
            if "massing" in text.lower():
                logger.info("concept_a loaded from %s (A.json not found)", p)
                return text
        raise RuntimeError(
            f"demo_inputs/A.json not found and no fallback located in {DEMO_INPUTS_DIR}. "
            "Clone the full repo so demo_inputs/ is present."
        )
    with open(path) as f:
        data = json.load(f)
    schedule = data.get("payload_schedule", [])
    return schedule[0]["payload"] if schedule else data["payload"]


def _fetch_embedding(text: str) -> list:
    url = f"http://{EMBEDDING_HOST}:{EMBEDDING_PORT}/v1/embeddings"
    resp = requests.post(
        url,
        json={"model": EMBEDDING_MODEL, "input": text},
        timeout=30,
    )
    resp.raise_for_status()
    return resp.json()["data"][0]["embedding"]


def _ensure_initialised():
    global _HERD_PAYLOAD_TEXT, _HERD_EMBEDDING, _initialised
    if _initialised:
        return
    with _init_lock:
        if _initialised:
            return
        _HERD_PAYLOAD_TEXT = _load_concept_a_payload()
        logger.info(
            "Thundering-herd payload: %.90s…", _HERD_PAYLOAD_TEXT
        )
        logger.info(
            "Fetching embedding from %s:%s …", EMBEDDING_HOST, EMBEDDING_PORT
        )
        _HERD_EMBEDDING = _fetch_embedding(_HERD_PAYLOAD_TEXT)
        logger.info(
            "Embedding ready (%d dims).  All users will conflict with each other.",
            len(_HERD_EMBEDDING),
        )
        _initialised = True


# ── Lazy proto imports ──────────────────────────────────────────────────────

dscc_pb2 = None
dscc_pb2_grpc = None


def _ensure_proto_modules():
    global dscc_pb2, dscc_pb2_grpc
    if dscc_pb2 is not None:
        return
    try:
        import dscc_pb2 as _pb2
        import dscc_pb2_grpc as _pb2_grpc
    except ImportError:
        raise ImportError(
            "Proto stubs not found.  Generate them once with:\n"
            "  python -m grpc_tools.protoc -I proto "
            "--python_out=. --grpc_python_out=. proto/dscc.proto"
        )
    dscc_pb2 = _pb2
    dscc_pb2_grpc = _pb2_grpc


# ── Custom metric helpers ───────────────────────────────────────────────────
# Locust surfaces custom numeric data by firing synthetic request events whose
# response_time carries the value of interest.  This makes queue_hops,
# wait_position, lock_wait_ms, and active_lock_count visible in the Web UI
# charts and --csv stats without any extra plugins.

def _fire_queue_metric(name: str, value: float):
    """Record a single thundering-herd queue metric as a Locust request event."""
    events.request.fire(
        request_type="ThunderingHerd",
        name=name,
        response_time=value,       # ← the metric value (ms, count, etc.)
        response_length=0,
        exception=None,
    )


# ── The Locust user ─────────────────────────────────────────────────────────

class ThunderingHerdUser(User):
    """
    Every user sends the same sustainability/massing-concept payload (A.json)
    with operation_type=WRITE, firing at maximum density so the server's
    semantic-lock queue fills up and agents begin hopping positions.

    Pacing
    ------
    * WAVE_PERIOD_S == 0 (default): fire continuously every ~200 ms.
      The lock_hold_ms=750 on the server means ≈3 requests arrive per lock
      period per user, keeping the queue deep.
    * WAVE_PERIOD_S > 0: fire one request immediately, then sleep for
      WAVE_PERIOD_S seconds.  This produces repeating crowd-rush spikes
      (visible in the Locust latency chart) that mirror the benchmark's
      discrete burst measurement.
    """

    def wait_time(self):
        if WAVE_PERIOD_S > 0:
            # Wave mode: the long sleep happens inside acquire_guard() itself;
            # Locust's wait_time is a minimum between tasks.
            return 0.001
        low  = max(0.02, (HERD_OP_INTERVAL_MS - HERD_OP_JITTER_MS) / 1000.0)
        high = max(low,  (HERD_OP_INTERVAL_MS + HERD_OP_JITTER_MS) / 1000.0)
        return random.uniform(low, high)

    def on_start(self):
        _ensure_proto_modules()
        _ensure_initialised()

        # Assign a permanent agent ID for this Locust user (the per-request
        # suffix distinguishes individual operations from the same user, just
        # as the benchmark's ordinal counter does).
        self._base_agent_id = _next_agent_id()
        self._seq = 0

        self.channel = grpc.insecure_channel(PROXY_TARGET)
        self.stub = dscc_pb2_grpc.LockServiceStub(self.channel)

        # NO startup stagger — this is the whole point.  The benchmark uses
        # ArrivalMode::kBurst which sets offset_for() == 0 for every agent.
        # All Locust users start firing immediately when spawned.
        logger.info(
            "ThunderingHerd user ready → agent_base=%s  proxy=%s",
            self._base_agent_id, PROXY_TARGET,
        )

    def on_stop(self):
        if hasattr(self, "channel"):
            self.channel.close()

    @task
    def acquire_guard(self):
        self._seq += 1
        agent_id = f"{self._base_agent_id}-op{self._seq}-{uuid.uuid4().hex[:4]}"

        request = dscc_pb2.AcquireRequest(
            agent_id=agent_id,
            embedding=_HERD_EMBEDDING,
            payload_text=_HERD_PAYLOAD_TEXT,
            source_file="A.json",
            timestamp_unix_ms=int(time.time() * 1000),
            operation_type=dscc_pb2.AcquireRequest.OPERATION_TYPE_WRITE,
        )

        start_ns = time.perf_counter_ns()
        exc = None
        response = None

        try:
            response = self.stub.AcquireGuard(request, timeout=60)
        except grpc.RpcError as e:
            exc = e

        elapsed_ms = (time.perf_counter_ns() - start_ns) / 1e6

        # ── Primary latency event (same structure as locustfile.py) ─────────
        events.request.fire(
            request_type="grpc",
            name="AcquireGuard/thundering_herd/write",
            response_time=elapsed_ms,
            response_length=0,
            exception=exc if exc else (
                None if (response and response.granted)
                else Exception(f"denied: {response.message if response else 'no response'}")
            ),
        )

        if response is None:
            # RPC error — no queue metrics to record
            if WAVE_PERIOD_S > 0:
                time.sleep(WAVE_PERIOD_S)
            return

        # ── Thundering-herd queue metrics ────────────────────────────────────
        # Each of these is surfaced as its own "request type" in the UI so
        # you can chart them independently alongside the latency histogram.
        _fire_queue_metric("queue_hops",        response.queue_hops)
        _fire_queue_metric("wait_position",     response.wait_position)
        _fire_queue_metric("lock_wait_ms",      response.lock_wait_ms)
        _fire_queue_metric("active_lock_count", response.active_lock_count)

        if LOG_HOPS:
            logger.info(
                "%-46s  granted=%-5s  wait_pos=%2d  hops=%2d  "
                "lock_wait=%5.0f ms  active_locks=%d",
                agent_id,
                response.granted,
                response.wait_position,
                response.queue_hops,
                response.lock_wait_ms,
                response.active_lock_count,
            )

        if WAVE_PERIOD_S > 0:
            # Sleep after reporting so the pause shows up in the think-time
            # gap between bursts, not in the request latency.
            time.sleep(WAVE_PERIOD_S)
