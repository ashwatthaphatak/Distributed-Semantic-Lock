# Final Review Presentation — Slide Rubric

**Course:** CSC 724  
**Title:** Concurrency Control for Shared Memory in Multi-Agent Systems  
**Format:** Introduction → Literature Review → Implementation → Experimental Results → Conclusion

---

## Section 1 — Introduction (3 slides)

### Slide 1 — Title
- Project title, authors, course, institution.

### Slide 2 — Motivation: The Shared Memory Problem
- Keep the semantic conflict illustration from the midsem review (three agents writing
  conflicting content to a shared vector database).
- Tighten the framing: as LLM-based multi-agent systems increasingly use shared vector
  memory (RAG pipelines, long-horizon agents), classical mutual exclusion is insufficient.
  Two writes can conflict semantically without being byte-identical.

### Slide 3 — Problem Statement and Contributions
- Problem: multi-agent systems lack a mechanism to prevent semantic race conditions in
  shared vector memory.
- Contributions:
  1. Design and implementation of a distributed semantic lock manager (DSLM) with Raft
     consensus, deployed as a 5-node C++ gRPC cluster.
  2. A Raft correctness fix (pending/promote model) that closes the admission-ordering
     gap identified at the midsem review.
  3. An empirical embedding model evaluation framed as a binary classification problem,
     comparing 3 models across paraphrase detection and cross-domain specificity.

---

## Section 2 — Literature Review (1 slide)

### Slide 4 — Related Work
One slide. Talk through each paper verbally — no need to put the explanations on the
slide itself. List the papers grouped by theme:

**Concurrency Control**
- Eswaran et al., "The Notions of Consistency and Predicate Locks in a Database System" (CACM 1976)
- Bernstein, Hadzilacos & Goodman, "Concurrency Control and Recovery in Database Systems" (1987)

**Distributed Lock Services**
- Burrows, "The Chubby Lock Service for Loosely-Coupled Distributed Systems" (OSDI 2006)
- Hunt et al., "ZooKeeper: Wait-free Coordination for Internet-Scale Systems" (ATC 2010)

**Consensus**
- Ongaro & Ousterhout, "In Search of an Understandable Consensus Algorithm" (USENIX ATC 2014)
- Ongaro, "Consensus: Bridging Theory and Practice" (PhD thesis, 2014)

**Multi-Agent Systems**
- Park et al., "Generative Agents: Interactive Simulacra of Human Behavior" (2023)
- Wu et al., "AutoGen: Enabling Next-Gen LLM Applications via Multi-Agent Conversation" (2023)

**Embeddings**
- Reimers & Gurevych, "Sentence-BERT: Sentence Embeddings using Siamese BERT-Networks" (EMNLP 2019)
- Chen et al., "BGE M3-Embedding" (2024)

**What to say for each cluster (speaker notes, not slide text):**
- *Concurrency control*: predicate locking (Eswaran) is the conceptual ancestor — DSLM
  generalizes the predicate from a key range to a cosine similarity threshold in embedding
  space. 2PL (Bernstein) is the protocol DSLM follows at the lock table level.
- *Distributed lock services*: Chubby and ZooKeeper solve *who holds the lock*, not
  *what content conflicts*. DSLM adds the semantic layer on top.
- *Consensus*: Raft (Ongaro & Ousterhout) is what we implemented. The pre-vote
  optimization from the thesis explains what you see in demo Test Case 3.
- *Multi-agent systems*: Generative Agents and AutoGen both write to shared memory but
  treat conflict prevention as out of scope. DSLM makes it in-scope and solves it.
- *Embeddings*: Sentence-BERT established cosine similarity as a semantic distance
  measure. BGE-M3 is one of the three models we evaluated.

---

## Section 3 — Implementation (4 slides)

### Slide 9 — System Architecture
- Full stack diagram: agent → embedding-service → dscc-proxy → 5-node Raft cluster →
  Qdrant.
- All components in C++ gRPC, built via CMake, deployed via Docker Compose.
- Proxy polls the cluster via `RaftService::GetLeader`; caches channels per backend;
  retries on NOT_LEADER / UNAVAILABLE / DEADLINE_EXCEEDED.

### Slide 10 — The Semantic Lock Table
- Per-lock FIFO waiter queues (not a global queue).
- Admission rule: compare incoming embedding against all active lock centroids using
  cosine similarity; block if any >= theta.
- Queue hops: when a blocker releases but a new conflict exists, the waiter is requeued
  behind the next conflicting lock (hop count increments).
- Live telemetry: `[LOCK_QUEUE]`, `[LOCK_REQUEUE]`, `[LOCK_GRANT]` log events with
  agent id, blocking agent, similarity, queue position, hops, theta.

### Slide 11 — Raft Integration: Pending/Promote Model
The corrected 11-step request lifecycle, closing the admission-ordering flaw identified
at the midsem review:

1. Client sends AcquireGuard to dscc-proxy.
2. Proxy resolves current leader via GetLeader.
3. Proxy forwards RPC to leader node.
4. Leader validates fields.
5. Leader checks IsLeader(); returns FAILED_PRECONDITION if not leader.
6. Leader calls `wait_for_admission` — blocks until no semantic conflict, then inserts a
   **pending** slot. The pending slot participates in conflict detection immediately,
   preventing any request from slipping through between admission and Raft commit.
7. Leader calls `Propose(ACQUIRE)` — replicates to quorum.
8. Leader calls `WaitUntilApplied` — blocks until apply callback promotes pending → real
   lock on all replicas.
9. Leader performs Qdrant operation (lock is now durably committed):
   - write: upsert vector + metadata
   - read: vector similarity query
10. Leader calls `Propose(RELEASE)` + `WaitUntilApplied` — releases lock, rebalances
    waiters.
11. RPC returns timing and trace metadata.

If Propose(ACQUIRE) fails: `remove_pending` cleans up the local pending slot; a
compensating RELEASE is appended via `AppendLocalEntry` to purge any partially
replicated ghost ACQUIRE from followers.

**Why this matters:** No Qdrant operation starts until step 8 completes. The pending slot
from step 6 blocks conflicting requests during the Raft commit window. This closes the
gap where the old design admitted locks locally before durable cluster agreement.

### Slide 12 — Benchmark and Demo Infrastructure
- **Locust load generator** (`locustfile.py`, `locustfile_base.py`): gRPC DSLM workload
  and direct Qdrant HTTP baseline, identical traffic shape per persona.
- **`compare_baseline.sh`**: runs both locust files back-to-back; outputs
  `overhead_comparison.png` and `overhead_summary.csv`.
- **`thundering_herd.py`**: 10-agent burst using threading.Barrier, mirrors the C++
  benchmark's burst arrival mode exactly.
- **`dscc-paraphrase-gauntlet-demo`**: 3 embedding models × 2 scenarios (paraphrase
  detection + cross-domain flood) × configurable theta; outputs confusion matrix JSON per
  model.
- **`dscc-benchmark`**: 13 curated scenarios in single / matrix / soak modes; outputs
  JSON and CSV.

---

## Section 4 — Experimental Results (4 slides)

### Slide R1 — The Price of Safety: Coordination Overhead
**Source:** `compare_baseline.sh` → `overhead_summary.csv` + `overhead_comparison.png`

Two Locust runs with identical traffic (5 agents A–E, same payloads, same pacing):
- **Baseline**: `locustfile_base.py` — direct Qdrant HTTP, no locking, no Raft, no proxy.
- **DSLM**: `locustfile.py` — full stack: gRPC client → proxy → leader hop → semantic
  conflict check → Raft ACQUIRE → Qdrant upsert → Raft RELEASE.

Show the p50 / p95 / p99 delta per agent/op endpoint (from `overhead_summary.csv`).

The delta is the **coordination cost** — the measurable price of semantic safety:
proxy hop + Raft replication + lock table check. Frame it as a question answered: *how
much does correctness cost, per operation, in milliseconds?*

### Slide R2 — Fault Tolerance: Raft Under Live Load
**Source:** Demo Test Cases 2, 3, 4 (live Locust load running throughout)

| Test | Action | Observed Result |
|------|--------|-----------------|
| TC2 | Kill Raft leader | Automatic failover; Locust p95 shows no visible latency spike |
| TC3 | Rejoin former leader | Pre-vote mechanism prevents unnecessary election disruption |
| TC4 | Kill 2 followers → kill 3rd → bring back 1 | Quorum: 2 failures tolerated (3/5); 3rd kill causes liveness loss; +1 node restores service |

**Framing:** Frame as *liveness vs safety* — the system never served incorrect results
under any fault scenario (C + P preserved). Availability was lost exactly when quorum
math says it must be. This is CAP theorem behaving correctly under load, validated
empirically with a live client workload visible in Locust.

### Slide R3 — Conflict Detection as a Classification Problem
**Source:** `src/paraphrase_gauntlet_demo.cpp` — confusion matrix framework

Frame conflict detection as a binary classifier:
- **Positive class**: two requests *should* conflict (cosine similarity >= theta → same-concept paraphrases).
- **Negative class**: two requests *should not* conflict (similarity < theta → cross-domain pairs).
- **Predicted positive**: system serialized them (lock-hold intervals do NOT overlap).
- **Predicted negative**: system ran them in parallel (lock-hold intervals DO overlap).

This yields a standard 2×2 confusion matrix:

|  | Serialized (predicted conflict) | Parallel (predicted no conflict) |
|--|--------------------------------|----------------------------------|
| **Should conflict** | TP | FN — serialization violation |
| **Should not conflict** | FP — wasted parallelism | TN |

Metrics:
- **Precision** = TP / (TP + FP) — of what was blocked, how much actually needed blocking.
- **Recall** = TP / (TP + FN) — of what needed blocking, how much was actually caught.
- **F1** — harmonic mean; penalizes both missed conflicts and false blocks equally.

A model with low FN but high FP is conservative: safe but slow. A model with high FN is
dangerous: fast but wrong. The ideal model maximizes F1 at the chosen operating theta.

### Slide R4 — Model Comparison Results
**Source:** `./build/dscc-paraphrase-gauntlet-demo` → `logs/paraphrase_gauntlet_results_<ts>.json`

Run the binary at theta=0.75 and theta=0.95 (via `DSLM_GAUNTLET_THETA`). Fill in the
table below from the `summary_comparison` field in the output JSON:

| Model | θ | TP | FN | FP | TN | Precision | Recall | F1 |
|-------|---|----|----|----|----|-----------|--------|----|
| all-minilm:latest | 0.75 | — | — | — | — | — | — | — |
| bge-m3:latest | 0.75 | — | — | — | — | — | — | — |
| qwen3-embedding:0.6b | 0.75 | — | — | — | — | — | — | — |
| all-minilm:latest | 0.95 | — | — | — | — | — | — | — |
| bge-m3:latest | 0.95 | — | — | — | — | — | — | — |
| qwen3-embedding:0.6b | 0.95 | — | — | — | — | — | — | — |

Annotate the best (model, θ) pair using the auto-generated `conclusion` field in the JSON.

Highlight two failure modes:
- **High FN** (missed conflicts): semantically similar writes ran concurrently — a
  correctness failure. Shows up as a low recall score.
- **High FP** (unnecessary blocks): unrelated writes were serialized — a throughput
  failure. Shows up as a low precision score.

---

## Section 5 — Conclusion (2 slides)

### Slide 13 — Conclusion
Four points:

1. **System**: Designed and implemented a distributed semantic lock manager with Raft
   consensus that enforces semantic serializability for concurrent multi-agent writes to a
   shared vector database.

2. **Correctness fix**: Closed the admission-ordering gap from the midsem review with the
   pending/promote model and ghost lock elimination via compensating RELEASE — no Qdrant
   operation starts before a lock is durably committed through Raft.

3. **Evaluation**: Empirically evaluated 3 embedding models using a binary classification
   framework (confusion matrix, precision, recall, F1) across paraphrase detection and
   cross-domain specificity scenarios at two similarity thresholds.

4. **Fault tolerance**: Demonstrated Raft quorum behavior under live load — automatic
   failover with no client-visible latency spike, correct liveness loss at quorum boundary,
   and recovery on node rejoin.

### Slide 14 — Open Problems and Future Work
- **Lease/TTL expiry**: no heartbeat-based cleanup for abandoned locks (agent crash leaves
  a lock held indefinitely).
- **Durable lock state**: active lock table is in-memory only; process restart loses all
  in-flight lock state with no recovery path.
- **Lock ownership enforcement**: any agent can currently release any lock; ownership
  validation on release is not implemented.
- **Theta auto-calibration**: threshold is selected manually; could be derived
  automatically from corpus similarity statistics.
- **Soak test**: the 2-hour DB-growth experiment (`DSLM_RUN_MODE=soak`) is implemented
  but not yet run to completion — would validate that serialization latency stays flat as
  the Qdrant collection scales.

---

## Slide Count Summary

| Section | Slides |
|---------|--------|
| 1 — Introduction | 3 |
| 2 — Literature Review | 1 |
| 3 — Implementation | 4 |
| 4 — Experimental Results | 4 |
| 5 — Conclusion | 2 |
| **Total** | **14** |

---

## Before the Demo

- Run `./build/dscc-paraphrase-gauntlet-demo` (or check `logs/`) to get the actual
  TP/FN/FP/TN numbers for Slide R4.
- Run `scripts/compare_baseline.sh` to get the overhead CSV and PNG for Slide R1.
- See `DEMO.md` for the full step-by-step demo script for Slides R1 and R2.
