"""
Baseline workload generator — directly targets Qdrant, bypassing the DSLM
proxy, Raft nodes, and semantic-conflict logic.

This file is the companion to ``locustfile.py``.  It emits the *same* agent
traffic (same personas, same pacing, same payloads, same pre-computed
embeddings) but routes every request straight to Qdrant's HTTP API instead of
the DSLM gRPC proxy.  Running both files back-to-back against an identical
workload lets you compute:

    coordination_overhead = latency(locustfile.py) - latency(locustfile_base.py)

which is the *price of safety* the DSLM stack adds on top of raw Qdrant:
gRPC hops (client→proxy→leader), semantic conflict search, Raft replication,
lock-table bookkeeping, and optional lock-hold time.

API-call matching
-----------------
To keep the comparison apples-to-apples, each op mirrors exactly what the
DSLM leader does in ``src/lock_service_impl.cpp``:

* **write** → ``PUT /collections/{col}/points?wait=true`` with one point
  (``id``, ``vector``, ``payload={agent_id, source_file, timestamp_unix_ms,
  raw_text}``).  ``wait=true`` forces Qdrant to wait for persistence, matching
  the leader's upsert.
* **read**  → ``POST /collections/{col}/points/search`` with the embedding
  vector, ``limit=3, with_payload=false, with_vector=false`` — identical to
  the leader's conflict-detection query.

Caveats when interpreting the results
-------------------------------------
* DSLM's write internally does **one search + one upsert**, the baseline
  write only does **one upsert**.  The extra search is real coordination
  cost and should be counted on the DSLM side.
* The DSLM response time includes ``LOCK_HOLD_MS`` (750 ms default in
  docker-compose.yml).  For a pure coordination-cost comparison, rerun the
  DSLM locust run with ``DSCC_LOCK_HOLD_MS=0``.
* Reads in DSLM still acquire a lock via Raft, so the per-read overhead will
  be large — that is expected and is the whole point of this comparison.

Setup
-----
    pip install locust requests

Usage (same host / flags as locustfile.py, just swap the file):

    locust -f locustfile_base.py --headless -u 5 -r 5 --run-time 5m

Environment variables
---------------------
    QDRANT_HOST                  Qdrant host   (default: localhost)
    QDRANT_PORT                  Qdrant port   (default: 6333)
    QDRANT_COLLECTION            Collection to hit
                                 (default: dscc_baseline — deliberately
                                  different from the DSLM default so the
                                  two runs never share state)
    BASELINE_RESET_COLLECTION    1 = DELETE + recreate the collection on
                                 first startup.  0 = create only if missing.
                                 (default: 0 — safer for reruns)

    EMBEDDING_HOST               Embedding host (default: localhost)
    EMBEDDING_PORT               Embedding port (default: 7997)
    EMBEDDING_MODEL              Model name     (default: all-minilm:latest)

    DSCC_AGENT_LABELS            Agents to include    (default: "ABCDE")
    DSCC_OP_INTERVAL_MS          Interval per user    (default: 1000)
    DSCC_OP_JITTER_MS            +/- jitter           (default: 200)
    DSCC_USE_FIRST_PAYLOAD_ONLY  1 = one payload per persona (default: 1)
"""

import glob
import json
import logging
import os
import random
import threading
import time
import uuid

import requests
from locust import User, task, events

logger = logging.getLogger(__name__)

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
DEMO_INPUTS_DIR = os.path.join(PROJECT_ROOT, "demo_inputs")

# ---- Qdrant ----------------------------------------------------------------
QDRANT_HOST = os.environ.get("QDRANT_HOST", "localhost")
QDRANT_PORT = os.environ.get("QDRANT_PORT", "6333")
QDRANT_COLLECTION = os.environ.get("QDRANT_COLLECTION", "dscc_baseline")
BASELINE_RESET_COLLECTION = os.environ.get("BASELINE_RESET_COLLECTION", "0") == "1"
QDRANT_BASE = f"http://{QDRANT_HOST}:{QDRANT_PORT}"

# ---- Embedding service (local to the client, same as locustfile.py) --------
EMBEDDING_HOST = os.environ.get("EMBEDDING_HOST", "localhost")
EMBEDDING_PORT = os.environ.get("EMBEDDING_PORT", "7997")
EMBEDDING_MODEL = os.environ.get("EMBEDDING_MODEL", "all-minilm:latest")

# ---- Workload shaping (mirrors locustfile.py / e2e_demo.cpp) ---------------
AGENT_LABELS = os.environ.get("DSCC_AGENT_LABELS", "ABCDE")
OP_INTERVAL_MS = int(os.environ.get("DSCC_OP_INTERVAL_MS", "1000"))
OP_JITTER_MS = int(os.environ.get("DSCC_OP_JITTER_MS", "200"))
USE_FIRST_PAYLOAD_ONLY = os.environ.get("DSCC_USE_FIRST_PAYLOAD_ONLY", "1") == "1"

AGENTS = []
EMBEDDING_CACHE = {}
EMBEDDING_DIM = 0
_init_lock = threading.Lock()
_initialised = False


# ---------------------------------------------------------------------------
# Agent loading (identical logic to locustfile.py so the generator matches)
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# Qdrant collection management (runs once, under _init_lock)
# ---------------------------------------------------------------------------

def _collection_exists():
    resp = requests.get(f"{QDRANT_BASE}/collections/{QDRANT_COLLECTION}", timeout=10)
    return resp.status_code == 200


def _create_collection(dim):
    body = {"vectors": {"size": dim, "distance": "Cosine"}}
    resp = requests.put(
        f"{QDRANT_BASE}/collections/{QDRANT_COLLECTION}",
        json=body, timeout=30,
    )
    # 200/201 = created, 409 = already exists (treat as success)
    if resp.status_code not in (200, 201, 409):
        raise RuntimeError(
            f"Failed to create Qdrant collection {QDRANT_COLLECTION}: "
            f"status={resp.status_code} body={resp.text}"
        )


def _delete_collection():
    requests.delete(f"{QDRANT_BASE}/collections/{QDRANT_COLLECTION}", timeout=30)


def _prepare_qdrant(dim):
    if BASELINE_RESET_COLLECTION:
        logger.info(
            "BASELINE_RESET_COLLECTION=1 → deleting %s before recreating",
            QDRANT_COLLECTION,
        )
        _delete_collection()
        _create_collection(dim)
        logger.info("Qdrant collection %s recreated (dim=%d)", QDRANT_COLLECTION, dim)
        return

    if _collection_exists():
        logger.info("Qdrant collection %s already exists — reusing", QDRANT_COLLECTION)
    else:
        _create_collection(dim)
        logger.info("Qdrant collection %s created (dim=%d)", QDRANT_COLLECTION, dim)


def _ensure_initialised():
    global AGENTS, EMBEDDING_CACHE, EMBEDDING_DIM, _initialised
    if _initialised:
        return
    with _init_lock:
        if _initialised:
            return
        AGENTS = _load_agents()
        if not AGENTS:
            raise RuntimeError(f"No agent JSON files found in {DEMO_INPUTS_DIR}")
        EMBEDDING_CACHE = _precompute_embeddings(AGENTS)
        EMBEDDING_DIM = len(next(iter(EMBEDDING_CACHE.values())))
        _prepare_qdrant(EMBEDDING_DIM)
        _initialised = True


# ---------------------------------------------------------------------------
# Round-robin persona assignment (same pattern as locustfile.py)
# ---------------------------------------------------------------------------

_agent_index = 0
_agent_index_lock = threading.Lock()


def _next_agent():
    global _agent_index
    with _agent_index_lock:
        idx = _agent_index % len(AGENTS)
        _agent_index += 1
    return AGENTS[idx]


# ---------------------------------------------------------------------------
# The user: one HTTP session per virtual user so connection setup cost is
# amortised the same way DSLM's leader amortises its Qdrant connection.
# ---------------------------------------------------------------------------

def _random_point_id():
    # Qdrant point IDs must be unsigned ints or UUID strings.  Use a random
    # positive int64 so we don't collide with DSLM's separate collection.
    return random.getrandbits(63)


class QdrantBaselineUser(User):
    """Emits the same persona-driven workload as ``locustfile.py`` but sends
    each op straight to Qdrant over HTTP."""

    def wait_time(self):
        low = max(0.05, (OP_INTERVAL_MS - OP_JITTER_MS) / 1000.0)
        high = max(low, (OP_INTERVAL_MS + OP_JITTER_MS) / 1000.0)
        return random.uniform(low, high)

    def on_start(self):
        _ensure_initialised()

        self.agent = _next_agent()
        self.session = requests.Session()
        self.seq = 0

        # Match the 200 ms startup stagger in e2e_demo.cpp.
        time.sleep(random.uniform(0, 0.2))

        logger.info(
            "Baseline user started as %s (%s) → Qdrant %s/%s, interval≈%dms",
            self.agent["name"], self.agent["role"],
            QDRANT_BASE, QDRANT_COLLECTION, OP_INTERVAL_MS,
        )

    def on_stop(self):
        if hasattr(self, "session"):
            self.session.close()

    # -- per-op helpers --------------------------------------------------

    def _do_write(self, agent_id, payload_text, embedding):
        body = {
            "points": [{
                "id": _random_point_id(),
                "vector": embedding,
                "payload": {
                    "agent_id": agent_id,
                    "source_file": f"{self.agent['label']}.json",
                    "timestamp_unix_ms": int(time.time() * 1000),
                    "raw_text": payload_text,
                },
            }],
        }
        url = f"{QDRANT_BASE}/collections/{QDRANT_COLLECTION}/points?wait=true"
        return self.session.put(url, json=body, timeout=30)

    def _do_read(self, embedding):
        body = {
            "vector": embedding,
            "limit": 3,
            "with_payload": False,
            "with_vector": False,
        }
        url = f"{QDRANT_BASE}/collections/{QDRANT_COLLECTION}/points/search"
        return self.session.post(url, json=body, timeout=30)

    # -- task ------------------------------------------------------------

    @task
    def qdrant_op(self):
        payload = random.choice(self.agent["payloads"])
        self.seq += 1
        agent_id = f"{self.agent['name'].lower()}-{self.seq}-{uuid.uuid4().hex[:6]}"
        embedding = EMBEDDING_CACHE[payload["text"]]

        name = f"QdrantDirect/{self.agent['label']}/{payload['operation']}"
        start_ns = time.perf_counter_ns()
        exc = None
        response_len = 0

        try:
            if payload["operation"] == "write":
                resp = self._do_write(agent_id, payload["text"], embedding)
            else:
                resp = self._do_read(embedding)
            response_len = len(resp.content) if resp.content else 0

            if resp.status_code >= 400:
                exc = Exception(
                    f"HTTP {resp.status_code}: {resp.text[:200]}"
                )
            else:
                # Qdrant wraps results in {"result": ..., "status": "ok"}
                try:
                    j = resp.json()
                    if j.get("status") not in (None, "ok"):
                        exc = Exception(f"qdrant status={j.get('status')}")
                except ValueError:
                    pass  # empty body on success is acceptable
        except requests.RequestException as e:
            exc = e

        elapsed_ms = (time.perf_counter_ns() - start_ns) / 1e6
        events.request.fire(
            request_type="http",
            name=name,
            response_time=elapsed_ms,
            response_length=response_len,
            exception=exc,
        )
