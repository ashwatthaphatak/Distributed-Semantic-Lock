# DSCC End-to-End Demo

## 1. Architecture

Current end-to-end flow:

`demo_inputs/*.txt -> Ollama embeddings -> dscc-node gRPC AcquireGuard -> ActiveLockTable -> Qdrant write -> lock release`

Main components:

- `src/e2e_bench.cpp`
  - the primary demo and validation harness
  - starts Docker services, requests embeddings, sends gRPC requests, validates Qdrant writes
- `src/lock_service_impl.cpp`
  - gRPC server implementation
  - applies semantic lock acquisition, writes vectors to Qdrant, releases the lock
- `src/active_lock_table.{h,cpp}`
  - in-memory semantic lock table
  - blocks requests whose cosine similarity is `>= theta`
- `docker-compose.yml`
  - runs:
    - `embedding-service` via `ollama/ollama:latest`
    - `qdrant`
    - `dscc-node`
- `demo_inputs/A.txt` through `demo_inputs/E.txt`
  - editable agent payloads used by the demo

Locking model:

- each request gets an embedding vector
- if its similarity to an active lock is `>= theta`, it waits
- once the write finishes, the lock is released

## 2. Build

Build the current end-to-end target:

```bash
cmake -S . -B /tmp/dslm_build
cmake --build /tmp/dslm_build --target dscc-e2e-bench -j"$(nproc)"
```

This is the main executable you should use for the project demo:

```bash
/tmp/dslm_build/dscc-e2e-bench
```

## 3. Run the Current Test Structure

The current testing structure is the real end-to-end bench. It does all of the following:

- starts Docker services
- ensures the Ollama embedding model is available
- reads `demo_inputs/*.txt`
- generates real embeddings
- prints pairwise similarity scores
- sends concurrent real `AcquireGuard` gRPC requests
- writes vectors and payload metadata into Qdrant
- validates Qdrant point counts and payloads

Run with defaults:

```bash
/tmp/dslm_build/dscc-e2e-bench
```

Run with automatic teardown after the demo:

```bash
E2E_TEARDOWN=1 /tmp/dslm_build/dscc-e2e-bench
```

Useful environment variables:

- `DSCC_THETA`
  - semantic conflict threshold
  - default: `0.78`
- `DSCC_LOCK_HOLD_MS`
  - how long the server keeps the lock after a successful write
  - default: `750`
- `EMBEDDING_IMAGE`
  - embedding service image
  - default: `ollama/ollama:latest`
- `EMBEDDING_MODEL_ID`
  - Ollama model used for embeddings
  - default: `all-minilm:latest`
- `QDRANT_COLLECTION`
  - Qdrant collection name
  - default: `dscc_memory_e2e`
- `E2E_TEARDOWN`
  - set to `1` to bring the stack down automatically on exit

Examples:

```bash
/tmp/dslm_build/dscc-e2e-bench
```

```bash
E2E_TEARDOWN=1 /tmp/dslm_build/dscc-e2e-bench
```

```bash
DSCC_THETA=0.30 /tmp/dslm_build/dscc-e2e-bench
```

```bash
DSCC_THETA=0.95 DSCC_LOCK_HOLD_MS=1000 /tmp/dslm_build/dscc-e2e-bench
```

## 4. Edit the Agent Inputs

The text files below are the actual payloads used by the demo:

- `demo_inputs/A.txt`
- `demo_inputs/B.txt`
- `demo_inputs/C.txt`
- `demo_inputs/D.txt`
- `demo_inputs/E.txt`

Change those files to test new semantic overlaps or non-overlaps.

Current intent of the seeded files:

- `A` and `B`
  - invoice-processing conflict pair
- `D` and `E`
  - payroll-summary conflict pair
- `C`
  - distinct server-reboot task

## 5. How to Read `dscc-e2e-bench`

The output is organized in this order:

### Runtime Configuration

Shows:

- project path
- embedding image and model
- `theta`
- lock hold time
- Qdrant collection

This tells you exactly what environment the run used.

### Pairwise Similarity Matrix

Example:

```text
       A   1.000   0.900   0.124   0.377   0.411
```

How to read it:

- each row/column is one text file
- each value is cosine similarity between two payloads
- if a value is greater than or equal to `theta`, those two agents are expected to conflict

Examples with the seeded files:

- `A-B = 0.900`
  - strong semantic overlap
- `D-E = 0.918`
  - strong semantic overlap
- `A-C = 0.124`
  - weak overlap

### Scenario Title

Each scenario header explains what the scenario is testing, for example:

- `Scenario One: Agents A and B describe the same invoice`
- `Scenario Two: Agents A, C, and D run together`

### Timeline

This is the most important part of the demo.

Each timeline line is printed in time order and shows when an agent:

- submitted text
- entered the embedding model
- left the embedding model
- entered DSLM
- reached the DSLM server
- acquired the semantic lock
- finished the Qdrant write
- released the semantic lock

When there is a conflict, the timeline will explicitly say:

- how long the agent waited
- which agent blocked it
- what similarity score caused the block
- which threshold was applied

Example interpretation:

```text
Agent A acquired semantic lock after waiting 754ms because Agent B matched at similarity 0.900 >= theta 0.300
```

This means:

- Agent A was blocked in the lock manager
- Agent B was the conflicting active lock
- the conflict score was `0.900`
- since `0.900 >= 0.300`, Agent A had to wait

### Agent Recap

Each agent gets a short recap with:

- embedding duration
- DSLM wait time
- blocking agent and similarity, if any
- Qdrant write-complete time
- final result

This is the easiest section to use in a live presentation.

### Qdrant Validation

After each scenario, the bench verifies:

- point count in Qdrant
- payload presence in Qdrant

If those checks pass, the vector DB write path worked for that scenario.

## 6. Threshold Tuning

The threshold controls how aggressively the semantic lock manager blocks requests.

Use:

```bash
DSCC_THETA=<value> /tmp/dslm_build/dscc-e2e-bench
```

Examples:

```bash
DSCC_THETA=0.95 /tmp/dslm_build/dscc-e2e-bench
DSCC_THETA=0.78 /tmp/dslm_build/dscc-e2e-bench
DSCC_THETA=0.30 /tmp/dslm_build/dscc-e2e-bench
DSCC_THETA=0.10 /tmp/dslm_build/dscc-e2e-bench
```

How lower thresholds change behavior:

- at `0.78`
  - only the strongest overlaps block
- at `0.30`
  - weaker semantic relationships can start blocking too
- at `0.10`
  - the lock manager becomes very aggressive

Important:

- if you lower `theta`, a scenario that used to be “distinct” may stop being distinct
- this is expected behavior, not necessarily a bug

## 7. Docker Behavior and Cleanup

Start the stack manually:

```bash
docker compose up -d --build qdrant embedding-service dscc-node
```

Stop and remove the stack:

```bash
docker compose down
```

Remove containers and volumes:

```bash
docker compose down -v
```

Current Qdrant persistence behavior:

- Qdrant does not mount a host data volume in this repo
- if the Qdrant container is removed, its data is effectively gone
- the bench also deletes the collection before each scenario

## 8. Layer Contract and Assumptions

Assumptions:

- the caller sends a valid `agent_id`, payload text, source file, timestamp, and embedding
- embeddings compared for overlap come from the same model and vector space
- Qdrant is reachable over HTTP
- semantic conflicts are decided by cosine similarity against `theta`

Responsibilities this layer does not take:

- no agent planning or orchestration
- no LLM reasoning layer
- no distributed coordination across multiple DSCC nodes
- no persistence of active lock state across process restarts

Upper-layer expectation:

- decide which text should be embedded
- supply meaningful agent identity
- choose a threshold that matches the desired blocking behavior

Lower-layer expectation:

- Qdrant accepts vector upserts and metadata payloads
- the embedding service returns stable fixed-dimension vectors
