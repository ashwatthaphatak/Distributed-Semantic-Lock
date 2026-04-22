"""
Realistic workload generator for the Distributed Semantic Lock Manager.

Simulates 13 AI agent personas sending AcquireGuard requests through the
gRPC proxy with organic timing variation.  Each Locust user adopts one
persona, picks a random payload from that persona's schedule on every
request, and waits a randomised interval before the next one.

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

    # 3b. Or headless — 13 users (one per persona), ramp 3/s, run 10 min:
    locust -f locustfile.py --headless -u 13 -r 3 --run-time 10m

Environment variables:

    DSCC_PROXY          gRPC proxy address       (default: localhost:50050)
    EMBEDDING_HOST      Embedding service host    (default: localhost)
    EMBEDDING_PORT      Embedding service port    (default: 7997)
    EMBEDDING_MODEL     Model name for embeddings (default: all-minilm:latest)
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
from locust import User, task, between, events

logger = logging.getLogger(__name__)

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
DEMO_INPUTS_DIR = os.path.join(PROJECT_ROOT, "demo_inputs")

PROXY_TARGET = os.environ.get("DSCC_PROXY", "localhost:50050")
EMBEDDING_HOST = os.environ.get("EMBEDDING_HOST", "localhost")
EMBEDDING_PORT = os.environ.get("EMBEDDING_PORT", "7997")
EMBEDDING_MODEL = os.environ.get("EMBEDDING_MODEL", "all-minilm:latest")

AGENTS = []
EMBEDDING_CACHE = {}
_init_lock = threading.Lock()
_initialised = False


def _load_agents():
    agents = []
    for path in sorted(glob.glob(os.path.join(DEMO_INPUTS_DIR, "*.json"))):
        with open(path) as f:
            data = json.load(f)
        label = os.path.splitext(os.path.basename(path))[0]

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

        agents.append({
            "label": label,
            "name": data["agent_name"],
            "role": data["role"],
            "payloads": payloads,
        })
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
    wait_time = between(2, 8)

    def on_start(self):
        _ensure_proto_modules()
        _ensure_initialised()

        self.agent = _next_agent()
        self.channel = grpc.insecure_channel(PROXY_TARGET)
        self.stub = dscc_pb2_grpc.LockServiceStub(self.channel)
        self.seq = 0
        logger.info(
            "User started as %s (%s) — %d payloads",
            self.agent["name"], self.agent["role"], len(self.agent["payloads"]),
        )

    def on_stop(self):
        if hasattr(self, "channel"):
            self.channel.close()

    @task
    def acquire_guard(self):
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
