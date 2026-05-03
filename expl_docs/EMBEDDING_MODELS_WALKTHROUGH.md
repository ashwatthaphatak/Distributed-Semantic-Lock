# Embedding Model Walkthrough

This walkthrough explains the three embedding models evaluated in DSLM, why the model
choice matters, how it changes the similarity threshold, and what changes in the
system's output when a client selects one model instead of another.

The intended use of this file is slide preparation. It is deliberately more explanatory
than the benchmark tables in `MODEL_FINDINGS.md`: it connects the model choice to the
semantic lock architecture, the threshold calibration story, and the client-facing
behavior of the system.

---

## 1. Why Embeddings Are an Architectural Choice

DSLM does not lock by row id, document id, or string equality. It locks by semantic proximity.

Every incoming request carries a natural-language payload. The embedding service converts that text into a numeric vector. The lock manager compares the incoming vector against the vectors for all active locks using cosine similarity:

```text
if cosine_similarity(incoming_embedding, active_lock_embedding) >= theta:
    block behind the most similar active lock
else:
    allow the operation to run concurrently
```

That means the embedding model is not a passive implementation detail. It defines the geometry of the lock space. The same two sentences can receive different cosine scores under different models. As a result, the model controls which operations are considered conflicting, which operations can run in parallel, how much queueing the system creates, and how the threshold should be interpreted.

In this system, the embedding model affects:

- **Correctness**: whether paraphrases of the same intent are serialized.
- **Throughput**: whether unrelated requests are allowed to run in parallel.
- **Latency**: both from embedding generation time and from extra lock waiting.
- **Threshold calibration**: the same theta value does not mean the same thing for every
  model.
- **Operational cost**: larger models require more memory and have higher steady-state
  embedding latency.
- **Client-visible behavior**: a client can see different lock waits, different ordering,different serialization outcomes, and different Qdrant write/search timing depending on the model selected.

The core lesson for slides: **theta is only meaningful relative to a specific embedding
model**. A threshold of `0.75` is not an absolute semantic rule. It is a decision boundary
inside one model's vector space.

---

## 2. Where the Model Sits in the Architecture

The embedding model is used before a request reaches the semantic conflict check.

```text
raw payload text
    -> embedding service /v1/embeddings
    -> vector<float> attached to AcquireRequest
    -> dscc-proxy forwards to current Raft leader
    -> ActiveLockTable compares vector to active lock centroids
    -> Raft ACQUIRE commit
    -> Qdrant write or read
    -> Raft RELEASE commit
```

The lock table does not know which model produced the vector. It only receives a vector and a threshold. Therefore, the client and deployment configuration must ensure that:

- all requests in one lock domain use the same embedding model,
- the Qdrant collection dimension matches the selected model,
- theta has been calibrated for that model,
- vectors from different models are not mixed in the same collection or lock epoch.

The evaluated models have different vector dimensions:

| Profile  |               Model ID | Vector Dimension | Benchmark Role                                    |
| -------- | ---------------------: | ---------------: | ------------------------------------------------- |
| `ollama` |    `all-minilm:latest` |              384 | Fast lightweight baseline                         |
| `bge`    |        `bge-m3:latest` |             1024 | Stronger multilingual / multi-granularity encoder |
| `qwen`   | `qwen3-embedding:0.6b` |             1024 | Highest-quality tested semantic detector          |

The current benchmark profiles are defined in `src/benchmark_runner.cpp`:

```cpp
{"ollama", "all-minilm:latest",       "ollama/ollama:latest"},
{"bge",    "bge-m3:latest",           "ollama/ollama:latest"},
{"qwen",   "qwen3-embedding:0.6b",    "ollama/ollama:latest"},
```

All three are served through the same Ollama HTTP endpoint. Model selection is per
embedding request via the `"model"` field.

---

## 3. The Three Models at a Glance

| Question                 | `all-minilm`                           | `bge-m3`                                                | `qwen3-embedding:0.6b`                                      |
| ------------------------ | -------------------------------------- | ------------------------------------------------------- | ----------------------------------------------------------- |
| What is it best for?     | Low-latency semantic approximation     | Broader semantic matching with richer representations   | Best tested correctness on paraphrase locking               |
| Vector size              | 384                                    | 1024                                                    | 1024                                                        |
| Median embedding latency | about 10 ms                            | about 111 ms                                            | about 133 ms                                                |
| Main strength            | Speed                                  | Stronger semantic coverage than MiniLM                  | Best balance of paraphrase recall and threshold calibration |
| Main risk                | Missed paraphrase conflicts            | Narrow calibration window, higher cold-path variability | Higher per-request embedding overhead                       |
| Best slide framing       | "Fast but unsafe for semantic locking" | "More capable, but not the winner here"                 | "Recommended model for this architecture"                   |

These labels are based on this project's benchmark corpus and scenarios. They should not
be read as universal rankings for every embedding workload. The important claim is more
specific: **for DSLM's lock/no-lock decision, `qwen3-embedding:0.6b` at theta `0.75`
performed best among the three tested options.**

---

## 4. Model 1: `all-minilm:latest`

### Why a Client Might Pick It

`all-minilm:latest` is the lightweight option. In the benchmark, its median embedding
latency was about `10 ms`, compared with about `111 ms` for `bge-m3` and `133 ms` for
`qwen3-embedding:0.6b`. It also produces 384-dimensional vectors, which lowers the cost
of vector transfer, storage, and cosine computation.

A client might pick it when:

- latency is much more important than semantic precision,
- the payloads are short and highly literal,
- conflict examples are near-identical rather than deeply paraphrased,
- hardware is constrained,
- the system is being used for demos, smoke tests, or low-risk environments,
- occasional missed semantic conflicts are acceptable.

In plain terms: `all-minilm` is attractive when the system needs to be fast and cheap,
and the semantic locking requirement is relatively forgiving.

### What Makes It Different

`all-minilm` is a compact sentence embedding model. Its smaller representation is enough
to capture broad sentence similarity, but in this project it did not reliably group
different paraphrases of the same operational intent above the chosen threshold.

That matters because DSLM's hardest job is not recognizing exact duplicates. Exact or
near-exact duplicates are easy. The harder case is recognizing that several agents are
writing semantically equivalent content with different wording.

Example conflict pattern:

```text
"prioritize passive cooling and low-carbon materials"
"evaluate daylight access and sustainable envelope decisions"
```

A human can see these are part of the same sustainability design concept. A weak
embedding model may place them close, but not close enough to cross theta. When that
happens, both writes can run at the same time.

### How It Affects the Threshold

For `all-minilm`, raising or lowering theta did not solve the paraphrase problem in the
benchmark:

| Theta | Paraphrase Gauntlet Serialization Score | Violations |
| ----: | --------------------------------------: | ---------: |
|  0.55 |                                   0.750 |          3 |
|  0.75 |                                   0.500 |          6 |
|  0.95 |                                   0.833 |          2 |

The non-monotonic-looking behavior is a reminder that the benchmark result is not just
"lower theta equals safer." The workload, expected conflict-pair accounting, active lock
timing, and queue dynamics all interact. The important practical point is simpler:
**no tested theta made `all-minilm` robust enough for the paraphrase-heavy workload.**

For slides, the threshold story is:

- `all-minilm` makes theta harder to tune because paraphrases are not consistently
  clustered.
- Lower theta can create more blocking, but that blocking is not necessarily the right
  blocking.
- Higher theta can miss paraphrases because the model does not push them close enough
  together.
- This model is fast, but the system may appear faster partly because it is failing to
  serialize some conflicts.

### How It Changes System Output

With `all-minilm`, clients should expect:

- lower embedding latency,
- lower per-vector storage and comparison cost,
- higher risk of false negatives, meaning related writes run concurrently,
- possible semantic race conditions in paraphrase-heavy workloads,
- deceptively good latency when missed conflicts avoid the queue,
- less reliable mapping between theta and human intuition.

In the output metrics, this shows up as:

- lower embedding p50,
- paraphrase violations,
- lower recall in conflict-detection framing,
- lock waits that may be low for the wrong reason.

### When It Is the Right Choice

Pick `all-minilm` only if the deployment values speed over semantic safety, or if the
client has validated that its real payloads are simple enough for this model. It is a
reasonable baseline and a useful performance floor. It should not be the default choice
for correctness-sensitive multi-agent shared memory.

---

## 5. Model 2: `bge-m3:latest`

### Why a Client Might Pick It

`bge-m3` is the middle option in this evaluation. It has a richer 1024-dimensional
representation and is designed for stronger semantic retrieval behavior than a compact
MiniLM-style baseline.

A client might pick it when:

- they need stronger semantic matching than `all-minilm`,
- the workload contains more varied language,
- multilingual or broader retrieval behavior matters,
- they can afford roughly 100 ms of steady-state embedding overhead,
- they want a conservative semantic model but are willing to tune theta carefully.

In architectural terms, `bge-m3` is the model you consider when `all-minilm` is clearly
too weak, but you still want to compare against another strong 1024-dimensional encoder
before choosing the final operating point.

### What Makes It Different

`bge-m3` produced stronger semantic overlap detection than `all-minilm` in some regions,
especially at lower theta. In Case 11, it scored `0.972` at theta `0.55`, much better
than `all-minilm` at the same threshold.

However, its behavior became less attractive as theta increased:

| Theta | Paraphrase Gauntlet Serialization Score | Violations |
| ----: | --------------------------------------: | ---------: |
|  0.55 |                                   0.972 |          1 |
|  0.75 |                                   0.750 |          3 |
|  0.95 |                                   0.667 |          4 |

This suggests that, for this corpus, `bge-m3` assigns paraphrases high but not always
extreme similarity. It can catch many conflicts if theta is permissive, but the useful
operating band is narrower.

### How It Affects the Threshold

For `bge-m3`, theta is sensitive:

- At theta `0.55`, paraphrase detection is strong, but cross-domain false-positive
  blocking becomes severe.
- At theta `0.75`, the false-positive latency penalty improves, but paraphrase
  serialization falls behind `qwen`.
- At theta `0.95`, the model becomes too strict for the paraphrase workload.

The central tradeoff is that `bge-m3` can be made conservative, but conservative settings
may serialize unrelated domains. That changes the slide framing:

```text
low theta with bge-m3:
    safer for paraphrases
    worse for useful parallelism

higher theta with bge-m3:
    better for parallelism
    worse for paraphrase recall
```

The model therefore requires careful threshold calibration on client-specific data. A
client should not assume that a theta selected for `qwen` transfers cleanly to `bge-m3`.

### How It Changes System Output

With `bge-m3`, clients should expect:

- higher embedding latency than `all-minilm`,
- 1024-dimensional vectors,
- stronger semantic grouping than `all-minilm` in permissive threshold ranges,
- possible false-positive blocking at low theta,
- possible missed paraphrases at high theta,
- more sensitivity to threshold selection.

In the output metrics, this can appear as:

- higher lock-wait P95 at low theta,
- lower distinct parallelism in cross-domain scenarios at low theta,
- good but not best paraphrase serialization,
- higher cold-path variability than the other tested models.

### When It Is the Right Choice

Pick `bge-m3` if the client wants a stronger model than `all-minilm`, has domain data to
calibrate theta, and is comfortable validating the false-positive/false-negative balance.
It is a credible model, but in this benchmark it was not the best final choice because
its strongest paraphrase result occurred at a theta that also caused unnecessary
cross-domain blocking.

---

## 6. Model 3: `qwen3-embedding:0.6b`

### Why a Client Might Pick It

`qwen3-embedding:0.6b` is the recommended model from this evaluation. It was the only
model-and-threshold combination that achieved a perfect Paraphrase Gauntlet
serialization score:

| Theta | Paraphrase Gauntlet Serialization Score | Violations |
| ----: | --------------------------------------: | ---------: |
|  0.55 |                                   0.875 |          3 |
|  0.75 |                                   1.000 |          0 |
|  0.95 |                                   0.917 |          1 |

A client should pick it when:

- correctness matters more than raw embedding speed,
- paraphrase detection is central to the workload,
- the system is guarding shared vector memory from semantic race conditions,
- client payloads are varied and natural-language-heavy,
- the deployment can afford about `133 ms` median embedding latency,
- the desired operating point is theta `0.75`.

The benchmark recommendation is:

```text
Use qwen3-embedding:0.6b at theta = 0.75.
```

### What Makes It Different

`qwen3-embedding:0.6b` produced the best tested alignment between the model's vector
space and DSLM's binary lock decision. At theta `0.75`, paraphrases were close enough to
serialize, while the cross-domain false-positive penalty observed at theta `0.55`
disappeared.

This is exactly what the architecture needs:

- same-concept paraphrases should map above theta,
- unrelated domains should map below theta,
- the selected theta should leave a usable margin on both sides,
- queueing should be caused by real semantic conflicts, not noise.

The model is not "best" because it is always faster. It is best here because its errors
are smallest at the operating point DSLM needs.

### How It Affects the Threshold

For `qwen3-embedding:0.6b`, theta `0.75` is the calibrated decision boundary in this
project's experiments.

At theta `0.55`, the model becomes too permissive. It detects broad relatedness and can
serialize cross-domain work that should remain parallel. In Case 12, this produced
severe false-positive blocking and high latency.

At theta `0.95`, the model becomes too strict. Some paraphrases no longer cross the
decision boundary, causing missed conflicts.

At theta `0.75`, it achieved the desired balance:

- Case 11: perfect paraphrase serialization, zero violations.
- Case 12: cross-domain false-positive latency penalty disappears.
- Case 13: reader-writer fairness remains architecture-level and model-agnostic.

For slides, this is the cleanest threshold story:

```text
theta too low:
    unrelated concepts become conflicts -> wasted parallelism

theta too high:
    paraphrases stop being conflicts -> semantic races

theta around 0.75 with qwen3:
    paraphrases serialize, unrelated domains can proceed
```

### How It Changes System Output

With `qwen3-embedding:0.6b`, clients should expect:

- higher embedding latency than `all-minilm`,
- 1024-dimensional vectors,
- strongest tested paraphrase conflict detection,
- fewer semantic overlap violations at theta `0.75`,
- more trustworthy lock waits because blocking corresponds to real conflicts,
- better slide-level explanation of theta as a calibrated operating point.

In output metrics, this shows up as:

- `serialization_score = 1.000` on the Paraphrase Gauntlet at theta `0.75`,
- `0` paraphrase violations at theta `0.75`,
- median embedding overhead around `133 ms`,
- system-level P95 latency close to the other models at theta `0.75`,
- no model-specific reader/writer fairness penalty.

### When It Is the Right Choice

Pick `qwen3-embedding:0.6b` for the default DSLM deployment when semantic correctness is
the main product requirement. It is the best fit for the project's claim: a distributed
lock manager that prevents semantic races in multi-agent shared vector memory.

---

## 7. How Model Choice Changes Theta

Theta is the cosine similarity threshold. It decides where "related enough to block"
begins.

However, cosine scores are model-specific. A theta of `0.75` under one model is not the
same semantic boundary as theta `0.75` under another model.

The threshold is affected by:

- the model's training objective,
- embedding dimensionality,
- how tightly the model clusters paraphrases,
- how far apart it pushes unrelated domains,
- whether it encodes surface vocabulary or deeper intent,
- the distribution of the client's payloads.

The practical effect is that the client is not choosing just a model. The client is
choosing a **model plus threshold pair**.

## 7.1 Threshold Too Low

When theta is too low, the system treats weakly related payloads as conflicts.

Consequences:

- more requests block,
- lock queues grow deeper,
- P95 and P99 latency increase,
- distinct parallelism falls,
- unrelated domains serialize unnecessarily,
- the system is safe but overly conservative.

This is a false-positive problem. It does not usually corrupt shared memory, but it
reduces throughput and makes the system feel slower than necessary.

## 7.2 Threshold Too High

When theta is too high, only near-identical payloads conflict.

Consequences:

- paraphrases may run concurrently,
- lock waits decrease,
- latency may look better,
- semantic race conditions become more likely,
- serialization score falls,
- recall falls in the classification framing.

This is a false-negative problem. It is more dangerous than a false positive because it
means the lock manager failed to guard shared memory.

## 7.3 Threshold Correctly Calibrated

When theta is well calibrated for the selected model:

- same-intent paraphrases serialize,
- unrelated domains remain parallel,
- lock wait is explainable,
- latency reflects real coordination cost,
- the model's embedding overhead is the main fixed cost,
- benchmark metrics line up with user intuition.

For this benchmark:

```text
recommended operating point = qwen3-embedding:0.6b + theta 0.75
```

---

## 8. How Model Choice Changes System Output

Changing the embedding model can change every downstream observable that depends on
semantic conflict detection.

## 8.1 Admission Decision

The same request pair may be:

- blocked under one model,
- admitted concurrently under another model.

This happens because each model returns a different vector and therefore a different
cosine score. The lock table is deterministic given the vector and theta, but model
choice changes the vector.

## 8.2 Queue Placement

When a conflict exists, the lock table blocks behind the active lock with the highest
similarity. A model can change which active lock is considered the strongest blocker.

That can change:

- queue order,
- queue depth,
- queue hops,
- lock wait time,
- which operation is released next.

## 8.3 Serialization Score

A stronger paraphrase model improves serialization score because it makes same-intent
operations cross theta more reliably.

For the Paraphrase Gauntlet at theta `0.75`:

| Model                  | Serialization Score | Violations |
| ---------------------- | ------------------: | ---------: |
| `all-minilm`           |               0.500 |          6 |
| `bge-m3`               |               0.750 |          3 |
| `qwen3-embedding:0.6b` |               1.000 |          0 |

This is the cleanest slide table for why model choice matters.

## 8.4 Distinct Parallelism

A model that over-scores unrelated domains will block too much. This lowers distinct
parallelism and raises latency.

At theta `0.55`, both larger models became too permissive in the cross-domain flood:

| Model                  | Cross-Domain Distinct Parallelism | Op Latency P95 |
| ---------------------- | --------------------------------: | -------------: |
| `bge-m3`               |                             0.000 |        5587 ms |
| `qwen3-embedding:0.6b` |                             0.000 |        5652 ms |

That result does not mean the larger models are bad. It means theta `0.55` is too low
for that workload.

## 8.5 Latency

Model choice affects latency in two separate ways.

First, there is direct embedding latency:

| Model                  | Embedding p50 | Embedding p95 | Embedding p99 |
| ---------------------- | ------------: | ------------: | ------------: |
| `all-minilm`           |         10 ms |         10 ms |        324 ms |
| `bge-m3`               |        111 ms |        160 ms |       1774 ms |
| `qwen3-embedding:0.6b` |        133 ms |        402 ms |       1517 ms |

Second, there is indirect lock-wait latency. A model that detects more conflicts causes
more queueing. That can be correct behavior if the conflicts are real, or wasted latency
if they are false positives.

Therefore, lower latency is not always better. A model may look fast because it failed to
block operations that should have been serialized.

## 8.6 Qdrant Collection Shape

The model changes vector dimension:

- `all-minilm`: 384 dimensions,
- `bge-m3`: 1024 dimensions,
- `qwen3-embedding:0.6b`: 1024 dimensions.

Qdrant collections are created with a fixed vector dimension. A deployment cannot safely
mix 384-dimensional and 1024-dimensional embeddings in the same collection. Changing the
model may require recreating or migrating the collection.

## 8.7 Client-Facing Interpretation

From a client's perspective, changing the model can change:

- whether `AcquireGuard` returns quickly or waits,
- how long `lock_wait_ms` is,
- whether two agents appear to run in parallel,
- whether writes arrive in Qdrant in serialized order,
- whether reads wait behind semantically related writes,
- whether benchmark output shows violations,
- whether the system looks safe, slow, permissive, or overly conservative.

This is why model selection should be part of the DSLM contract. The client is not merely
choosing an embedding provider. The client is choosing the semantic conflict detector.

---

## 9. Model Selection Guidance

## 9.1 If the Client Wants Maximum Speed

Choose `all-minilm:latest` only if correctness risk is acceptable.

Use it for:

- demos,
- small local tests,
- fast baselines,
- workloads with very literal duplicate text,
- environments where missed paraphrase conflicts are low impact.

Avoid it for:

- high-stakes shared memory,
- paraphrase-heavy multi-agent writing,
- final correctness claims,
- clients that expect semantic rather than lexical conflict detection.

## 9.2 If the Client Wants a Stronger General Encoder

Choose `bge-m3:latest` if the client has time to calibrate theta on representative data.

Use it for:

- comparison against another strong model,
- broader semantic matching,
- workloads where low-theta conservatism is acceptable,
- cases where client-specific validation prefers BGE behavior.

Avoid it when:

- the system needs the best measured result from this benchmark,
- the threshold cannot be tuned,
- cross-domain false positives are costly.

## 9.3 If the Client Wants the Best DSLM Default

Choose `qwen3-embedding:0.6b` at theta `0.75`.

Use it for:

- correctness-sensitive deployments,
- paraphrase-heavy workloads,
- final presentation results,
- semantic race prevention,
- the default recommended architecture.

Accept the tradeoff:

- about `133 ms` median embedding overhead,
- 1024-dimensional vectors,
- higher compute/memory requirement than `all-minilm`.

---

## 10. Slide-Ready Narrative

A concise slide sequence could be:

## Slide A: The Model Is the Lock Predicate

Main point:

```text
DSLM does not ask "same key?"
DSLM asks "same meaning?"
The embedding model defines what "same meaning" means.
```

Visual:

```text
Text -> Embedding Model -> Vector -> Cosine Similarity -> theta -> Block / Admit
```

Speaker note:

The lock manager itself only sees numbers. If the model places two paraphrases far apart,
the distributed system will faithfully allow them to run in parallel. Consensus cannot
fix a bad semantic predicate.

## Slide B: Theta Is Model-Specific

Main point:

```text
theta = 0.75 is not universal.
It only has meaning inside one model's vector space.
```

Use this contrast:

```text
too low  -> false positives -> safe but slow
too high -> false negatives -> fast but unsafe
```

Speaker note:

The correct threshold is not selected in isolation. The deployable unit is
`(embedding model, theta)`.

## Slide C: Three Models, Three Tradeoffs

Use this table:

| Model        | Best Reason to Pick     | Main Risk                   |
| ------------ | ----------------------- | --------------------------- |
| `all-minilm` | Very fast               | Misses paraphrase conflicts |
| `bge-m3`     | Richer semantic encoder | Sensitive theta calibration |
| `qwen3`      | Best tested correctness | Higher embedding overhead   |

Speaker note:

The model choice is a safety/latency tradeoff. The fast model is not automatically the
best model because missed conflicts can masquerade as performance.

## Slide D: Result That Decides the Recommendation

Use the theta `0.75` Paraphrase Gauntlet table:

| Model        | Serialization Score | Violations |
| ------------ | ------------------: | ---------: |
| `all-minilm` |               0.500 |          6 |
| `bge-m3`     |               0.750 |          3 |
| `qwen3`      |               1.000 |          0 |

Speaker note:

This is the direct correctness test: can the model detect paraphrases of the same
operation? Only `qwen3` at theta `0.75` had zero violations.

## Slide E: Recommendation

Main point:

```text
Recommended default: qwen3-embedding:0.6b at theta = 0.75
```

Why:

- perfect paraphrase serialization in Case 11,
- no false-positive latency penalty at theta `0.75` in Case 12,
- reader/writer fairness remains architecture-level,
- embedding overhead is higher but bounded.

Speaker note:

The selected model is not the cheapest model. It is the model whose vector space gives
the lock manager the most reliable conflict predicate.

---

## 11. Final Recommendation

For the final presentation, frame the result as a model-calibrated concurrency-control
decision:

```text
Embedding model + theta = semantic lock predicate
```

Then make the recommendation:

```text
Use qwen3-embedding:0.6b with theta = 0.75 for DSLM's default semantic locking mode.
```

The reason is not just that `qwen3` is a stronger embedding model in the abstract. The
reason is that, in this architecture, it produced the best boundary between:

- paraphrases that must serialize, and
- unrelated domains that should remain parallel.

That boundary is the heart of the system. Raft ensures all nodes agree on lock state, but
the embedding model decides what counts as a conflict in the first place.
