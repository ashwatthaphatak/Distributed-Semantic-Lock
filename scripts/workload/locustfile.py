# Author: Ayush Gala
"""
Workload generator for the Distributed Semantic Lock Manager.

Mirrors the workload emitted by ``src/e2e_demo.cpp`` so Locust generates the
same dense semantic-conflict traffic:

* Only agents A-E participate by default.  These personas have overlapping
  payloads (A<->B massing concept, D<->E atrium phasing, etc.), which drives
  frequent cosine-similarity conflicts.
* Each user sticks to the FIRST payload in its agent's ``payload_schedule``
  (matching the ``load_template`` behaviour in ``e2e_demo.cpp``).  Reusing a
  single text per persona maximises the conflict rate.
* Users fire AcquireGuard roughly every 1 second with small jitter, like the
  C++ demo's ``DEMO_OP_INTERVAL_MS`` (default 1000ms).

Setup (one-time):

    pip install locust grpcio grpcio-tools requests

    # Generate Python proto stubs from the project root:
    python -m grpc_tools.protoc \
        -I proto \
        --python_out=. \
        --grpc_python_out=. \
        proto/dscc.proto

Usage:

    # 1. Start the Docker stack yourself:
    docker compose up -d --build

    # 2. Wait for Qdrant + embedding service + nodes to be healthy.

    # 3a. Run Locust with the web UI (http://localhost:8089):
    locust -f locustfile.py

    # 3b. Or headless — 5 users (one per A-E persona), ramp 5/s, run 5 min:
    locust -f locustfile.py --headless -u 5 -r 5 --run-time 5m

Environment variables:

    DSCC_PROXY                    gRPC proxy address        (default: localhost:50050)
    EMBEDDING_HOST                Embedding service host    (default: localhost)
    EMBEDDING_PORT                Embedding service port    (default: 7997)
    EMBEDDING_MODEL               Model name for embeddings (default: all-minilm:latest)
    DSCC_AGENT_LABELS             Agents to include         (default: "ABCDE", use "" for all)
    DSCC_OP_INTERVAL_MS           Target interval per user  (default: 1000)
    DSCC_OP_JITTER_MS             +/- jitter around interval (default: 200)
    DSCC_USE_FIRST_PAYLOAD_ONLY   1 = always use first payload (default: 1)
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

import pathlib as _pathlib
PROJECT_ROOT = str(_pathlib.Path(__file__).resolve().parents[2])
DEMO_INPUTS_DIR = os.path.join(PROJECT_ROOT, "demo_inputs")

PROXY_TARGET = os.environ.get("DSCC_PROXY", "localhost:50050")
EMBEDDING_HOST = os.environ.get("EMBEDDING_HOST", "localhost")
EMBEDDING_PORT = os.environ.get("EMBEDDING_PORT", "7997")
EMBEDDING_MODEL = os.environ.get("EMBEDDING_MODEL", "all-minilm:latest")

# Conflict-dense workload controls — defaults mirror src/e2e_demo.cpp.
#
# AGENT_LABELS restricts which demo personas participate.  The default "ABCDE"
# matches the label set used by e2e_demo.cpp and contains deliberate payload
# overlaps (A<->B, D<->E) that drive semantic conflicts.  Set to "" (empty) to
# include every *.json file in demo_inputs/ (the old 13-agent behaviour).
AGENT_LABELS = os.environ.get("DSCC_AGENT_LABELS", "ABCDE")

# Target interval between ops per user, with optional jitter.  Matches
# DEMO_OP_INTERVAL_MS (default 1000ms) in e2e_demo.cpp.
OP_INTERVAL_MS = int(os.environ.get("DSCC_OP_INTERVAL_MS", "1000"))
OP_JITTER_MS = int(os.environ.get("DSCC_OP_JITTER_MS", "200"))

# When truthy, every user always sends the FIRST payload in its agent's
# payload_schedule, exactly like load_template() in e2e_demo.cpp.  Reusing a
# single text per persona keeps conflicts maximally dense.  Set to 0 to fall
# back to the previous behaviour of picking a random payload per request.
USE_FIRST_PAYLOAD_ONLY = os.environ.get("DSCC_USE_FIRST_PAYLOAD_ONLY", "1") == "1"

AGENTS = []
EMBEDDING_CACHE = {}
_init_lock = threading.Lock()
_initialised = False


def _load_agents():
    label_filter = set(AGENT_LABELS) if AGENT_LABELS else None

    agents = []
    for path in sorted(glob.glob(os.path.join(DEMO_INPUTS_DIR, "*.json"))):
        label = os.path.splitext(os.path.basename(path))[0]
        if label_filter is not None and label not in label_filter:
            continue
        with open(path) as f:
            data = json.load(f)

        payloads = []
        for entry in data.get("payload_schedule", []):
            payloads.append({
                "text": entry["payload"],
                "operation": entry.get("operation", data.get("operation", "write")),
            })
        if not payloads:
            payloads.append({
                "text": data["payload"],
                "operation": data.get("operation", "write"),
            })

        # Mirror e2e_demo.cpp's load_template(): collapse the schedule down to
        # its first entry so every request from this persona reuses the same
        # text.  This keeps semantic conflicts maximally dense.
        if USE_FIRST_PAYLOAD_ONLY:
            payloads = payloads[:1]

        agents.append({
            "label": label,
            "name": data["agent_name"],
            "role": data["role"],
            "payloads": payloads,
        })

    if label_filter is not None:
        missing = sorted(label_filter - {a["label"] for a in agents})
        if missing:
            logger.warning(
                "Requested agent labels not found in %s: %s",
                DEMO_INPUTS_DIR, ",".join(missing),
            )
    return agents


def _get_embedding(text):
    url = f"http://{EMBEDDING_HOST}:{EMBEDDING_PORT}/v1/embeddings"
    resp = requests.post(url, json={"model": EMBEDDING_MODEL, "input": text}, timeout=30)
    resp.raise_for_status()
    return resp.json()["data"][0]["embedding"]


def _precompute_embeddings(agents):
    unique_texts = set()
    for agent in agents:
        for payload in agent["payloads"]:
            unique_texts.add(payload["text"])

    cache = {}
    logger.info("Pre-computing embeddings for %d unique payloads...", len(unique_texts))
    for i, text in enumerate(sorted(unique_texts), 1):
        cache[text] = _get_embedding(text)
        logger.info("  [%d/%d] %s...", i, len(unique_texts), text[:70])
    logger.info("All embeddings cached.")
    return cache


def _ensure_initialised():
    global AGENTS, EMBEDDING_CACHE, _initialised
    if _initialised:
        return
    with _init_lock:
        if _initialised:
            return
        AGENTS = _load_agents()
        if not AGENTS:
            raise RuntimeError(f"No agent JSON files found in {DEMO_INPUTS_DIR}")
        EMBEDDING_CACHE = _precompute_embeddings(AGENTS)
        _initialised = True


# Lazy-import the generated proto modules so the file can be parsed even if
# the stubs haven't been generated yet (the import error surfaces at runtime
# only when the test actually starts).
dscc_pb2 = None
dscc_pb2_grpc = None


def _ensure_proto_modules():
    global dscc_pb2, dscc_pb2_grpc
    if dscc_pb2 is not None:
        return
    import sys as _sys
    _proto_root = str(_pathlib.Path(__file__).resolve().parents[2])
    if _proto_root not in _sys.path:
        _sys.path.insert(0, _proto_root)
    try:
        import dscc_pb2 as _pb2
        import dscc_pb2_grpc as _pb2_grpc
    except ImportError:
        raise ImportError(
            "Proto stubs not found.  Generate them with:\n"
            "  python -m grpc_tools.protoc -I proto "
            "--python_out=. --grpc_python_out=. proto/dscc.proto"
        )
    dscc_pb2 = _pb2
    dscc_pb2_grpc = _pb2_grpc


_agent_index = 0
_agent_index_lock = threading.Lock()


def _next_agent():
    """Round-robin agent assignment so each user gets a distinct persona."""
    global _agent_index
    with _agent_index_lock:
        idx = _agent_index % len(AGENTS)
        _agent_index += 1
    return AGENTS[idx]


class DSLMUser(User):
    # Tight, near-constant pacing that matches the thread loop in e2e_demo.cpp
    # (``std::this_thread::sleep_for(interval_ms)`` per request).  Falls back
    # to a minimum of 50 ms if the configured window would go non-positive.
    def wait_time(self):
        low = max(0.05, (OP_INTERVAL_MS - OP_JITTER_MS) / 1000.0)
        high = max(low, (OP_INTERVAL_MS + OP_JITTER_MS) / 1000.0)
        return random.uniform(low, high)

    def on_start(self):
        _ensure_proto_modules()
        _ensure_initialised()

        self.agent = _next_agent()
        self.channel = grpc.insecure_channel(PROXY_TARGET)
        self.stub = dscc_pb2_grpc.LockServiceStub(self.channel)
        self.seq = 0

        # Stagger user startup the same way e2e_demo.cpp launches its worker
        # threads 200 ms apart, so we don't get a thundering-herd at t=0.
        time.sleep(random.uniform(0, 0.2))

        logger.info(
            "User started as %s (%s) — %d payload(s), interval≈%dms",
            self.agent["name"], self.agent["role"],
            len(self.agent["payloads"]), OP_INTERVAL_MS,
        )

    def on_stop(self):
        if hasattr(self, "channel"):
            self.channel.close()

    @task
    def acquire_guard(self):
        # When USE_FIRST_PAYLOAD_ONLY is set (default), self.agent["payloads"]
        # has been trimmed to length 1, so this picks the single canonical
        # text for this persona — matching e2e_demo.cpp.
        payload = random.choice(self.agent["payloads"])
        self.seq += 1
        agent_id = (
            f"{self.agent['name'].lower()}-{self.seq}-{uuid.uuid4().hex[:6]}"
        )

        embedding = EMBEDDING_CACHE[payload["text"]]
        op_type = (
            dscc_pb2.AcquireRequest.OPERATION_TYPE_WRITE
            if payload["operation"] == "write"
            else dscc_pb2.AcquireRequest.OPERATION_TYPE_READ
        )

        request = dscc_pb2.AcquireRequest(
            agent_id=agent_id,
            embedding=embedding,
            payload_text=payload["text"],
            source_file=f"{self.agent['label']}.json",
            timestamp_unix_ms=int(time.time() * 1000),
            operation_type=op_type,
        )

        start_ns = time.perf_counter_ns()
        try:
            response = self.stub.AcquireGuard(request, timeout=30)
            elapsed_ms = (time.perf_counter_ns() - start_ns) / 1e6

            if response.granted:
                events.request.fire(
                    request_type="grpc",
                    name=f"AcquireGuard/{self.agent['label']}/{payload['operation']}",
                    response_time=elapsed_ms,
                    response_length=0,
                    exception=None,
                )
            else:
                events.request.fire(
                    request_type="grpc",
                    name=f"AcquireGuard/{self.agent['label']}/{payload['operation']}",
                    response_time=elapsed_ms,
                    response_length=0,
                    exception=Exception(f"denied: {response.message}"),
                )
        except grpc.RpcError as e:
            elapsed_ms = (time.perf_counter_ns() - start_ns) / 1e6
            events.request.fire(
                request_type="grpc",
                name=f"AcquireGuard/{self.agent['label']}/{payload['operation']}",
                response_time=elapsed_ms,
                response_length=0,
                exception=e,
            )
