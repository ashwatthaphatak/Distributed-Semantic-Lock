# DSCC — Detailed Low-Level Architecture

---

## 1. Full System Topology

```
                                  ┌─────────────────────────────────────────────────────────────┐
                                  │                    HOST MACHINE                              │
                                  │                                                              │
                                  │  ┌────────────────────────┐  ┌────────────────────────────┐ │
                                  │  │  dscc-e2e-bench (C++)  │  │  dscc-benchmark (C++)      │ │
                                  │  │  Host-side harness     │  │  10-scenario stress runner  │ │
                                  │  │  - loads demo_inputs/  │  │  - curated workloads        │ │
                                  │  │  - calls Ollama for    │  │  - live queue-event stream  │ │
                                  │  │    embeddings          │  │  - correctness validation   │ │
                                  │  │  - gRPC to proxy       │  │  - JSON log output          │ │
                                  │  │  - validates Qdrant    │  │  - gRPC to proxy            │ │
                                  │  └──────────┬─────────────┘  └──────────┬─────────────────┘ │
                                  │             │ HTTP :7997                │                    │
                                  │             ▼                           │                    │
                                  │  ┌────────────────────────┐            │                    │
                                  │  │  Ollama (Docker)       │            │                    │
                                  │  │  all-minilm:latest     │            │                    │
                                  │  │  Port 7997 → 11434    │            │                    │
                                  │  │  384-dim float vectors │            │                    │
                                  │  └────────────────────────┘            │                    │
                                  │                                        │                    │
                                  └────────────────────────────────────────┼────────────────────┘
                                                                           │
                                            gRPC :50050                    │ gRPC :50050
                         ┌─────────────────────────────────────────────────┘
                         │
                         ▼
        ┌────────────────────────────────────────────────────────────────────────────────────┐
        │                            DOCKER COMPOSE NETWORK                                  │
        │                                                                                    │
        │  ┌──────────────────────────────────────────────────────────────────────────────┐  │
        │  │                        dscc-proxy (C++ gRPC)                                 │  │
        │  │                        Port 50050                                            │  │
        │  │                                                                              │  │
        │  │  ProxyServiceImpl ──────────────────────────────────────────────────────────  │  │
        │  │  │  LeaderPollLoop thread          ForwardWithLeaderRetry<Req,Resp,Fn>     │  │  │
        │  │  │  ├─ polls GetLeader RPC         ├─ 3-attempt retry loop                │  │  │
        │  │  │  │  on all 5 backends           ├─ extracts leader-address metadata    │  │  │
        │  │  │  │  every 100ms                 ├─ follows NOT_LEADER redirects        │  │  │
        │  │  │  └─ caches current_leader_      └─ 35s per-attempt deadline            │  │  │
        │  │  │                                                                         │  │  │
        │  │  │  channels_: map<string, grpc::Channel>  (cached per backend address)    │  │  │
        │  │  └─────────────────────────────────────────────────────────────────────────  │  │
        │  └────┬─────────┬──────────┬──────────┬──────────┬─────────────────────────────┘  │
        │       │         │          │          │          │                                  │
        │       │ gRPC    │ gRPC     │ gRPC     │ gRPC     │ gRPC                            │
        │       │ :50051  │ :50052   │ :50053   │ :50054   │ :50055                           │
        │       ▼         ▼          ▼          ▼          ▼                                  │
        │  ┌─────────┐┌─────────┐┌─────────┐┌─────────┐┌─────────┐                          │
        │  │ node-1  ││ node-2  ││ node-3  ││ node-4  ││ node-5  │                          │
        │  │ :50051  ││ :50052  ││ :50053  ││ :50054  ││ :50055  │  ◄── LockService         │
        │  │ :50061  ││ :50062  ││ :50063  ││ :50064  ││ :50065  │  ◄── RaftService         │
        │  └────┬────┘└────┬────┘└────┬────┘└────┬────┘└────┬────┘                          │
        │       │          │          │          │          │                                  │
        │       └──────────┴────┬─────┴──────────┴──────────┘                                │
        │                       │                                                             │
        │                       │ Full-mesh Raft gRPC (AppendEntries, RequestVote)            │
        │                       │ Ports 50061–50065                                           │
        │                       │                                                             │
        │  ┌────────────────────┴───────────────────────────────────────────────────────────┐ │
        │  │                                                                                │ │
        │  │   ┌──────────────────┐                          ┌──────────────────┐           │ │
        │  │   │   Qdrant         │                          │   Ollama         │           │ │
        │  │   │   Port 6333      │                          │   Port 7997      │           │ │
        │  │   │   Collection:    │                          │   → 11434        │           │ │
        │  │   │   dscc_memory_e2e│                          │   all-minilm     │           │ │
        │  │   │   384-dim Cosine │                          │   384-dim        │           │ │
        │  │   └──────────────────┘                          └──────────────────┘           │ │
        │  │                                                                                │ │
        │  └────────────────────────────────────────────────────────────────────────────────┘ │
        └────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Single dscc-node Internal Architecture

Each of the 5 `dscc-node` processes is identical. One is elected Raft leader; the others are followers.

```
┌──────────────────────────────────────────────────────────────────────────────────────────────┐
│                              dscc-node process                                                │
│                                                                                               │
│  ┌─── gRPC Server ──────────────────────────────────────────────────────────────────────────┐ │
│  │  Listening on TWO ports:                                                                 │ │
│  │    Port A (e.g. 50051)  →  LockService   (client-facing: Ping, AcquireGuard, Release)   │ │
│  │    Port B (e.g. 50061)  →  RaftService   (inter-node: RequestVote, AppendEntries, ...)  │ │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                               │
│  ┌─── LockServiceImpl ──────────────────────────────────────────────────────────────────────┐ │
│  │                                                                                          │ │
│  │  Dependencies:  RaftNode* raft_              (nullable for single-node mode)             │ │
│  │                 ActiveLockTable* lock_table_                                             │ │
│  │                                                                                          │ │
│  │  Config:        node_id_        (string, e.g. "node-1")                                 │ │
│  │                 theta_          (float, default 0.85, from THETA env)                    │ │
│  │                 lock_hold_ms_   (int, from LOCK_HOLD_MS env)                             │ │
│  │                 raft_propose_timeout_ms_  (int, default 5000)                            │ │
│  │                 qdrant_host_    (string, e.g. "qdrant")                                  │ │
│  │                 qdrant_port_    (string, e.g. "6333")                                    │ │
│  │                 qdrant_collection_  (string, e.g. "dscc_memory_e2e")                    │ │
│  │                                                                                          │ │
│  │  Methods:       AcquireGuard()  →  THE critical request path (see §4)                   │ │
│  │                 ReleaseGuard()  →  standalone release via Raft proposal                  │ │
│  │                 Ping()          →  health check, returns "pong from <node_id>"           │ │
│  │                                                                                          │ │
│  │  Qdrant I/O:    send_http_json()           →  raw TCP socket, hand-built HTTP/1.1       │ │
│  │                 upsert_embedding_to_qdrant()  →  PUT /collections/.../points?wait=true  │ │
│  │                 query_embedding_from_qdrant()  →  POST /collections/.../points/search   │ │
│  │                 ensure_qdrant_collection()  →  PUT /collections/<name> (idempotent)      │ │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                               │
│  ┌─── ActiveLockTable ──────────────────────────────────────────────────────────────────────┐ │
│  │                                                                                          │ │
│  │  Data:   unordered_map<string, SemanticLock>  active_     (keyed by agent_id)            │ │
│  │          mutex                                 mu_        (protects all state)           │ │
│  │                                                                                          │ │
│  │  API:    acquire(agent_id, embedding, θ)   →  blocks caller if conflict, returns trace  │ │
│  │          release(agent_id)                 →  removes lock, rebalances waiters           │ │
│  │          apply_acquire(agent_id, emb, θ)   →  insert without conflict check (Raft path) │ │
│  │          apply_release(agent_id)           →  delegates to release()                    │ │
│  │          size() / active_count()           →  number of active locks                    │ │
│  │          active_agent_ids()                →  sorted list of held lock agent IDs         │ │
│  │                                                                                          │ │
│  │  Internal:                                                                               │ │
│  │          find_conflict_locked(emb, θ)      →  O(n) scan, returns strongest conflict     │ │
│  │          apply_acquire_locked(id, emb, θ)  →  insert/update in active_ map              │ │
│  │          remove_lock_locked(id)            →  erase from active_, return waiters deque   │ │
│  │          rebalance_waiters_locked(...)     →  re-check each waiter, grant or requeue    │ │
│  │          cosine_similarity(a, b)           →  double-precision dot/(||a||·||b||)        │ │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                               │
│  ┌─── RaftNode ─────────────────────────────────────────────────────────────────────────────┐ │
│  │                                                                                          │ │
│  │  State:  current_term_           (int64, monotonically increasing)                      │ │
│  │          voted_for_              (string, candidate voted for in current term)           │ │
│  │          log_                    (vector<LogEntry>, index-0 is sentinel with term=0)     │ │
│  │          commit_index_           (int64, highest committed entry)                       │ │
│  │          last_applied_           (int64, highest applied entry)                         │ │
│  │          state_                  (FOLLOWER | CANDIDATE | LEADER)                        │ │
│  │          current_leader_id_      (string)                                               │ │
│  │          current_leader_address_ (string, the client-facing host:port of the leader)    │ │
│  │          next_index_             (map<peer_addr, int64>)                                 │ │
│  │          match_index_            (map<peer_addr, int64>)                                 │ │
│  │          peers_                  (map<peer_addr, PeerConnection{grpc::Channel}>)        │ │
│  │          election_deadline_      (steady_clock time_point)                              │ │
│  │                                                                                          │ │
│  │  Threads (3):                                                                            │ │
│  │    ┌──────────────────────────────────────────────────────────────────────────┐           │ │
│  │    │ ElectionTimerLoop                                                       │           │ │
│  │    │   polls every 10ms; if state != LEADER and now >= election_deadline_    │           │ │
│  │    │   → calls StartElection()                                               │           │ │
│  │    └──────────────────────────────────────────────────────────────────────────┘           │ │
│  │    ┌──────────────────────────────────────────────────────────────────────────┐           │ │
│  │    │ HeartbeatLoop                                                           │           │ │
│  │    │   if state == LEADER: spawn N threads to ReplicateToFollower(peer)      │           │ │
│  │    │   sleeps config_.heartbeat_ms (75ms) between rounds                     │           │ │
│  │    └──────────────────────────────────────────────────────────────────────────┘           │ │
│  │    ┌──────────────────────────────────────────────────────────────────────────┐           │ │
│  │    │ ApplyLoop                                                               │           │ │
│  │    │   waits on apply_cv_ until last_applied_ < commit_index_               │           │ │
│  │    │   increments last_applied_, calls on_commit_(log_[last_applied_])       │           │ │
│  │    │   notifies apply_cv_ after each apply (for WaitUntilApplied callers)   │           │ │
│  │    └──────────────────────────────────────────────────────────────────────────┘           │ │
│  │                                                                                          │ │
│  │  Key Methods:                                                                            │ │
│  │    Propose(entry, timeout, &index)    →  leader appends to log_, replicates, waits      │ │
│  │    WaitUntilApplied(index, timeout)   →  blocks until ApplyLoop reaches index           │ │
│  │    StartElection()                    →  increments term, sends RequestVote to all peers │ │
│  │    ReplicateToFollower(peer)          →  sends AppendEntries, handles rollback           │ │
│  │    AdvanceCommitIndexLocked()         →  majority match_index → new commit_index        │ │
│  │    HandleRequestVote(req) → resp                                                        │ │
│  │    HandleAppendEntries(req) → resp                                                      │ │
│  │    HandleGetLeader() → LeaderInfo                                                       │ │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                               │
│  ┌─── RaftServiceImpl ──────────────────────────────────────────────────────────────────────┐ │
│  │  Thin gRPC wrapper — delegates every RPC to the RaftNode methods:                       │ │
│  │    RequestVote()      →  raft_->HandleRequestVote()                                     │ │
│  │    AppendEntries()    →  raft_->HandleAppendEntries()                                   │ │
│  │    InstallSnapshot()  →  raft_->HandleInstallSnapshot()                                 │ │
│  │    GetLeader()        →  raft_->HandleGetLeader()                                       │ │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                               │
│  ┌─── Wiring (main.cpp) ───────────────────────────────────────────────────────────────────┐ │
│  │  1. Read all config from environment variables                                          │ │
│  │  2. Create ActiveLockTable (stack-allocated)                                            │ │
│  │  3. Create RaftNode with on_commit_ lambda:                                             │ │
│  │       ACQUIRE → lock_table.apply_acquire(agent_id, embedding, theta)                    │ │
│  │       RELEASE → lock_table.apply_release(agent_id)                                      │ │
│  │  4. raft.Start()  — spawns 3 background threads                                        │ │
│  │  5. Create LockServiceImpl, RaftServiceImpl                                             │ │
│  │  6. gRPC ServerBuilder: add TWO listening ports, register BOTH services                 │ │
│  │  7. server->Wait()  — blocks main thread                                               │ │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Data Structures — Lock Table Internals

```
┌─ ActiveLockTable ──────────────────────────────────────────────────────────────────────────┐
│                                                                                            │
│  mu_: std::mutex   ◄── guards ALL reads/writes to active_                                 │
│                                                                                            │
│  active_: unordered_map<string, SemanticLock>                                              │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  Key: agent_id (e.g. "sustainability_agent_0")                                     │   │
│  │                                                                                     │   │
│  │  Value: SemanticLock                                                                │   │
│  │  ┌────────────────────────────────────────────────────────────────────────────────┐  │   │
│  │  │  agent_id:   string       (lock owner)                                        │  │   │
│  │  │  centroid:   vector<float> (384 floats, 1,536 bytes)                          │  │   │
│  │  │  threshold:  float         (θ used when this lock was acquired)               │  │   │
│  │  │                                                                                │  │   │
│  │  │  waiters: deque<shared_ptr<WaitQueueEntry>>   ◄── per-lock FIFO queue         │  │   │
│  │  │  ┌──────────────────────────────────────────────────────────────────────────┐  │  │   │
│  │  │  │  WaitQueueEntry [0]                                                     │  │  │   │
│  │  │  │  ┌───────────────────────────────────────────────────────────────────┐   │  │  │   │
│  │  │  │  │  waiting_agent_id:  string                                       │   │  │  │   │
│  │  │  │  │  embedding:         vector<float>  (384 floats)                  │   │  │  │   │
│  │  │  │  │  theta:             float                                        │   │  │  │   │
│  │  │  │  │  cv:                shared_ptr<condition_variable>  ◄── per-waiter│   │  │  │   │
│  │  │  │  │  ready:             bool  (set true to unblock cv.wait)          │   │  │  │   │
│  │  │  │  │  granted:           bool  (true = ownership transferred by       │   │  │  │   │
│  │  │  │  │                            releaser, skip re-acquire)            │   │  │  │   │
│  │  │  │  │  queue_hops:        int   (requeue count across lock chains)     │   │  │  │   │
│  │  │  │  └───────────────────────────────────────────────────────────────────┘   │  │  │   │
│  │  │  │  WaitQueueEntry [1] ...                                                 │  │  │   │
│  │  │  │  WaitQueueEntry [N] ...                                                 │  │  │   │
│  │  │  └──────────────────────────────────────────────────────────────────────────┘  │  │   │
│  │  └────────────────────────────────────────────────────────────────────────────────┘  │   │
│  │                                                                                     │   │
│  │  (more entries for other active agents...)                                          │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
│  AcquireTrace (returned to caller after acquire completes)                                │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  waited:                   bool                                                    │   │
│  │  blocking_similarity_score: float  (highest cosine sim that caused blocking)       │   │
│  │  blocking_agent_id:        string  (agent whose lock caused the block)             │   │
│  │  wait_position:            int     (position in first queue entered)               │   │
│  │  wake_count:               int     (times cv was signaled)                         │   │
│  │  queue_hops:               int     (times requeued behind different locks)         │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
│  ConflictTrace (internal, from find_conflict_locked)                                      │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  lock:        SemanticLock*  (pointer to strongest conflicting active lock, or null)│   │
│  │  similarity:  float          (cosine similarity that triggered the conflict)       │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. The AcquireGuard Critical Path (Leader Only)

This is the most important code path in the system. Every agent request flows through here on the Raft leader.

```
Client/Proxy
    │
    │  AcquireGuard(agent_id, embedding[384], payload_text, source_file, operation_type)
    ▼
┌─── LockServiceImpl::AcquireGuard ─────────────────────────────────────────────────────────┐
│                                                                                            │
│  ① VALIDATE INPUTS                                                                        │
│     agent_id empty?  → return granted=false, "agent_id is required"                       │
│     embedding empty? → return granted=false, "embedding is required"                      │
│     Normalize operation_type (UNSPECIFIED → WRITE)                                        │
│     Record server_received_unix_ms = now()                                                │
│                                                                                            │
│  ② CHECK LEADERSHIP                                                                       │
│     if (raft_ != null && !raft_->IsLeader())                                              │
│       → set leader_redirect from raft_->LeaderAddress()                                   │
│       → add "leader-address" to gRPC trailing metadata                                    │
│       → return FAILED_PRECONDITION "NOT_LEADER"                                           │
│                                                                                            │
│  ③ SEMANTIC ADMISSION (may block)                                                         │
│     ┌──────────────────────────────────────────────────────────────────────────────────┐   │
│     │  lock_table_->acquire(agent_id, embedding, theta_)                              │   │
│     │                                                                                  │   │
│     │  (See §5 for full algorithm — this call holds mu_ and may cv.wait)              │   │
│     │                                                                                  │   │
│     │  Returns: AcquireTrace { waited, blocking_sim, blocking_agent, position, ...}   │   │
│     └──────────────────────────────────────────────────────────────────────────────────┘   │
│     Record lock_acquired_unix_ms = now()                                                  │
│     Populate response telemetry fields from AcquireTrace                                  │
│                                                                                            │
│  ④ RAFT PROPOSE ACQUIRE                                                                   │
│     Build LogEntry { term=current, op=ACQUIRE, agent_id, embedding[384], theta }          │
│     raft_->Propose(entry, 5000ms, &acquire_log_index)                                     │
│     If quorum fails → lock_table_->release(agent_id), return UNAVAILABLE                  │
│                                                                                            │
│  ⑤ QDRANT OPERATION (under lock)                                                          │
│     ┌────────────────────────────┐  ┌────────────────────────────────────────────┐         │
│     │  WRITE path               │  │  READ path                                 │         │
│     │  point_id = FNV-1a hash   │  │  POST /collections/.../points/search       │         │
│     │    (agent_id ⊕ timestamp) │  │  body: {vector:[...], limit:3,             │         │
│     │  PUT /collections/.../    │  │         with_payload:false}                 │         │
│     │    points?wait=true       │  │  Single attempt, no retry                  │         │
│     │  body: {points:[{         │  │                                             │         │
│     │    id, vector, payload:{  │  └────────────────────────────────────────────┘         │
│     │      agent_id, source_   │                                                          │
│     │      file, timestamp,    │                                                          │
│     │      raw_text}}]}        │                                                          │
│     │  3 retries, 75ms×attempt │                                                          │
│     │    backoff               │                                                          │
│     └────────────────────────────┘                                                         │
│     Record qdrant_write_complete_unix_ms = now()                                          │
│                                                                                            │
│  ⑥ LOCK HOLD (writes only)                                                               │
│     if operation == WRITE && lock_hold_ms > 0:                                            │
│       sleep_for(lock_hold_ms)    ◄── simulates extended critical section                  │
│                                                                                            │
│  ⑦ RAFT PROPOSE RELEASE                                                                   │
│     Build LogEntry { term=current, op=RELEASE, agent_id }                                 │
│     raft_->Propose(entry, 5000ms, &release_log_index)                                     │
│     raft_->WaitUntilApplied(release_log_index, 5000ms)                                    │
│       → blocks until ApplyLoop calls on_commit_ for this index                            │
│       → on_commit_ calls lock_table_->apply_release(agent_id)                             │
│       → which calls release() → removes lock, rebalances waiters                          │
│     Record lock_released_unix_ms = now()                                                  │
│                                                                                            │
│  ⑧ SAFETY NET: ScopeExit guard                                                           │
│     If any exit path is taken before ⑦ completes, the destructor fires                   │
│     commit_release_once() to prevent lock leaks                                           │
│                                                                                            │
│  ⑨ RETURN                                                                                 │
│     response: granted=true, message="write/read granted and committed"                    │
│     + all timing fields + all queue telemetry fields                                      │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 5. Lock Acquire Algorithm (ActiveLockTable::acquire)

```
acquire(agent_id, embedding, threshold):
    ┌─────────────────────────────────────────────────────────────────────┐
    │  unique_lock<mutex> lock(mu_)                                      │
    │  granted_by_handoff = false                                        │
    │                                                                     │
    │  LOOP:                                                              │
    │  ┌───────────────────────────────────────────────────────────────┐  │
    │  │  conflict = find_conflict_locked(embedding, threshold)       │  │
    │  │                                                               │  │
    │  │  find_conflict_locked:                                        │  │
    │  │    FOR EACH (agent_id, SemanticLock) in active_:              │  │
    │  │      sim = cosine_similarity(embedding, lock.centroid)        │  │
    │  │      if sim >= threshold && sim > best.similarity:            │  │
    │  │        best = {&lock, sim}                                    │  │
    │  │    RETURN best    (nullptr if no conflict)                    │  │
    │  │                                                               │  │
    │  │  IF conflict.lock == nullptr:                                 │  │
    │  │    ┌─────────────────────────────────────────────────────────┐│  │
    │  │    │ NO CONFLICT — grant immediately                        ││  │
    │  │    │ apply_acquire_locked(agent_id, embedding, threshold)   ││  │
    │  │    │   → active_[agent_id] = SemanticLock{id, emb, θ, []}  ││  │
    │  │    │   (or update centroid if agent_id already exists)      ││  │
    │  │    │ BREAK out of loop                                      ││  │
    │  │    └─────────────────────────────────────────────────────────┘│  │
    │  │                                                               │  │
    │  │  ELSE:                                                        │  │
    │  │    ┌─────────────────────────────────────────────────────────┐│  │
    │  │    │ CONFLICT — must wait                                   ││  │
    │  │    │ Create WaitQueueEntry:                                  ││  │
    │  │    │   waiting_agent_id = agent_id                          ││  │
    │  │    │   embedding = embedding                                ││  │
    │  │    │   theta = threshold                                    ││  │
    │  │    │   cv = make_shared<condition_variable>()               ││  │
    │  │    │                                                         ││  │
    │  │    │ conflict.lock->waiters.push_back(waiter)               ││  │
    │  │    │ Emit [LOCK_QUEUE] log event                            ││  │
    │  │    │                                                         ││  │
    │  │    │ Record trace: blocking_sim, blocking_agent, position   ││  │
    │  │    │                                                         ││  │
    │  │    │ waiter->cv->wait(lock, []{return waiter->ready;})      ││  │
    │  │    │   ◄── BLOCKS HERE, releases mu_, re-acquires on wake  ││  │
    │  │    │                                                         ││  │
    │  │    │ After wake:                                             ││  │
    │  │    │   waiter->ready = false                                 ││  │
    │  │    │   ++trace.wake_count                                   ││  │
    │  │    │   trace.queue_hops = waiter->queue_hops                ││  │
    │  │    │                                                         ││  │
    │  │    │ IF waiter->granted:                                     ││  │
    │  │    │   granted_by_handoff = true                             ││  │
    │  │    │   BREAK  (releaser already inserted us into active_)   ││  │
    │  │    │ ELSE:                                                   ││  │
    │  │    │   CONTINUE loop (re-check for conflicts)               ││  │
    │  │    └─────────────────────────────────────────────────────────┘│  │
    │  └───────────────────────────────────────────────────────────────┘  │
    │                                                                     │
    │  unlock(mu_)                                                        │
    │  if (!granted_by_handoff) print_active_locks()                     │
    │  RETURN trace                                                       │
    └─────────────────────────────────────────────────────────────────────┘
```

---

## 6. Lock Release and Waiter Rebalance Algorithm

```
release(agent_id):
    ┌────────────────────────────────────────────────────────────────────────────────┐
    │  lock_guard<mutex> lock(mu_)                                                  │
    │                                                                                │
    │  waiters = remove_lock_locked(agent_id)                                        │
    │    → active_.erase(agent_id)                                                  │
    │    → return the waiter deque that was attached to this lock                    │
    │                                                                                │
    │  rebalance_waiters_locked(waiters, granted_waiters):                           │
    │  ┌──────────────────────────────────────────────────────────────────────────┐  │
    │  │  WHILE waiters is not empty:                                             │  │
    │  │    waiter = waiters.pop_front()                                          │  │
    │  │                                                                          │  │
    │  │    conflict = find_conflict_locked(waiter.embedding, waiter.theta)       │  │
    │  │                                                                          │  │
    │  │    IF conflict exists:                                                   │  │
    │  │      ┌──────────────────────────────────────────────────────────────┐    │  │
    │  │      │  Still blocked — requeue behind new conflicting lock        │    │  │
    │  │      │  waiter.ready = false                                       │    │  │
    │  │      │  waiter.granted = false                                     │    │  │
    │  │      │  ++waiter.queue_hops                                        │    │  │
    │  │      │  conflict.lock->waiters.push_back(waiter)                   │    │  │
    │  │      │  Emit [LOCK_REQUEUE] log event                             │    │  │
    │  │      └──────────────────────────────────────────────────────────────┘    │  │
    │  │                                                                          │  │
    │  │    ELSE:                                                                 │  │
    │  │      ┌──────────────────────────────────────────────────────────────┐    │  │
    │  │      │  No conflict — grant via ownership handoff                  │    │  │
    │  │      │  apply_acquire_locked(waiter.agent_id, emb, θ)             │    │  │
    │  │      │  waiter.granted = true                                      │    │  │
    │  │      │  waiter.ready = true                                        │    │  │
    │  │      │  Emit [LOCK_GRANT] log event                               │    │  │
    │  │      │  Add waiter to granted_waiters list                        │    │  │
    │  │      └──────────────────────────────────────────────────────────────┘    │  │
    │  └──────────────────────────────────────────────────────────────────────────┘  │
    │                                                                                │
    │  unlock(mu_)                                                                   │
    │                                                                                │
    │  FOR EACH waiter in granted_waiters:                                           │
    │    waiter->cv->notify_all()    ◄── wake ONLY the granted waiters              │
    │                                     (no thundering herd)                       │
    └────────────────────────────────────────────────────────────────────────────────┘
```

---

## 7. Raft Consensus Internals

### 7.1 Raft State Machine

```
                     ┌───────────────────────────────────────────────────────────────┐
                     │                     Raft FSM                                   │
                     │                                                                │
  election           │   ┌───────────┐  timeout  ┌─────────────┐  majority  ┌──────┐│
  timeout            │   │ FOLLOWER  │──────────▶│ CANDIDATE   │──────────▶│LEADER││
  (600-1000ms)       │   │           │           │             │           │      ││
                     │   │ - receives│           │ - increments│           │ -sends││
                     │   │   AppendE.│           │   term      │           │  heart││
                     │   │ - votes   │           │ - votes for │           │  beats││
                     │   │ - applies │           │   self      │           │ -repli││
                     │   │   commits │           │ - sends     │           │  cates││
                     │   │           │◀──────────│   RequestV. │◀──────────│      ││
                     │   └───────────┘  higher   └─────────────┘  higher  └──────┘│
                     │                   term                      term            │
                     │                                                                │
                     │   Transitions on higher term:  any state → FOLLOWER           │
                     └───────────────────────────────────────────────────────────────┘
```

### 7.2 Raft Log Structure

```
log_: vector<LogEntry>

Index:  0 (sentinel)     1              2              3              4        ...
      ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
      │ term: 0  │  │ term: 1  │  │ term: 1  │  │ term: 1  │  │ term: 2  │
      │ op: ---  │  │ op: ACQ  │  │ op: REL  │  │ op: ACQ  │  │ op: REL  │
      │          │  │ agent:   │  │ agent:   │  │ agent:   │  │ agent:   │
      │ sentinel │  │  "agt_A" │  │  "agt_A" │  │  "agt_C" │  │  "agt_C" │
      │          │  │ emb:[384]│  │          │  │ emb:[384]│  │          │
      │          │  │ θ: 0.78  │  │          │  │ θ: 0.78  │  │          │
      └──────────┘  └──────────┘  └──────────┘  └──────────┘  └──────────┘
                                                       ▲              ▲
                                                       │              │
                                              commit_index_    last entry
                                              (quorum agreed)

Each AcquireGuard RPC produces exactly 2 log entries: ACQUIRE + RELEASE
ACQUIRE entries carry the full 384-float embedding (1,536 bytes each)
RELEASE entries carry only the agent_id
```

### 7.3 Replication Flow (Propose)

```
Leader calls Propose(entry, timeout=5s):

    ┌─────────────────────────────────────────────────────────────────────────┐
    │  1. Append entry to local log_ with current_term_                      │
    │     proposed_index = log_.size() - 1                                   │
    │                                                                         │
    │  2. LOOP until timeout:                                                 │
    │     ┌───────────────────────────────────────────────────────────────┐   │
    │     │  Spawn N threads (one per peer):                             │   │
    │     │    ReplicateToFollower(peer_address):                        │   │
    │     │      ┌─────────────────────────────────────────────────────┐ │   │
    │     │      │ Build AppendRequest:                                │ │   │
    │     │      │   term, leader_id, leader_service_address          │ │   │
    │     │      │   prev_log_index = next_index_[peer] - 1           │ │   │
    │     │      │   prev_log_term = log_[prev_log_index].term()      │ │   │
    │     │      │   entries = log_[next_index_[peer] .. end]          │ │   │
    │     │      │   leader_commit = commit_index_                    │ │   │
    │     │      │                                                     │ │   │
    │     │      │ Send via gRPC (150ms timeout)                      │ │   │
    │     │      │                                                     │ │   │
    │     │      │ On success:                                         │ │   │
    │     │      │   update match_index_[peer]                        │ │   │
    │     │      │   update next_index_[peer]                         │ │   │
    │     │      │   AdvanceCommitIndexLocked()                       │ │   │
    │     │      │     → scan indices from end, count replicated >= 3 │ │   │
    │     │      │     → if quorum: commit_index_ = index            │ │   │
    │     │      │     → notify commit_cv_ and apply_cv_             │ │   │
    │     │      │                                                     │ │   │
    │     │      │ On failure (log inconsistency):                    │ │   │
    │     │      │   Use conflict_term/conflict_index hints           │ │   │
    │     │      │   Decrement next_index_[peer], retry in same call  │ │   │
    │     │      └─────────────────────────────────────────────────────┘ │   │
    │     │  Join all threads                                            │   │
    │     │                                                               │   │
    │     │  Check: commit_index_ >= proposed_index?                     │   │
    │     │    YES → return true (committed)                              │   │
    │     │    NO  → wait 25ms on commit_cv_, continue loop              │   │
    │     └───────────────────────────────────────────────────────────────┘   │
    │                                                                         │
    │  3. Timeout reached without quorum → return false                      │
    └─────────────────────────────────────────────────────────────────────────┘
```

### 7.4 Election Flow

```
StartElection():
    ┌───────────────────────────────────────────────────────────────────────────┐
    │  lock(mu_)                                                                │
    │    state_ = CANDIDATE                                                    │
    │    ++current_term_                                                        │
    │    voted_for_ = self                                                     │
    │    clear leader info                                                     │
    │    reset election_deadline_ = now + random(600, 1000)ms                  │
    │  unlock(mu_)                                                              │
    │                                                                           │
    │  votes = 1  (self-vote)                                                  │
    │                                                                           │
    │  FOR EACH peer (sequentially):                                           │
    │    Send RequestVote:                                                      │
    │      { term, candidate_id=self, last_log_index, last_log_term }          │
    │      timeout: 150ms                                                      │
    │                                                                           │
    │    Follower checks:                                                      │
    │      term < current? → reject                                            │
    │      term > current? → become follower of new term                       │
    │      already voted for someone else? → reject                            │
    │      candidate log not up-to-date? → reject                              │
    │        IsLogUpToDateLocked:                                              │
    │          compare last_log_term first, then last_log_index                │
    │      otherwise → grant vote, reset own election timer                    │
    │                                                                           │
    │    If response.term > current_term_ → become follower, abort             │
    │    If vote_granted → ++votes                                             │
    │    If votes >= quorum (3 of 5) → BecomeLeaderLocked()                   │
    │      → initialize next_index_ = log.size for all peers                   │
    │      → initialize match_index_ = 0 for all peers                        │
    └───────────────────────────────────────────────────────────────────────────┘
```

---

## 8. Proxy Architecture

```
┌─── ProxyServiceImpl ─────────────────────────────────────────────────────────────────────────┐
│                                                                                               │
│  Config:                                                                                      │
│    backend_nodes_:      ["node-1:50051", "node-2:50052", ..., "node-5:50055"]               │
│    leader_poll_ms_:     100   (how often to poll GetLeader)                                  │
│    request_timeout_ms_: 35000 (deadline per forwarded RPC, allows for lock_hold + Raft)      │
│    leader_rpc_timeout_ms_: 750 (deadline for GetLeader poll RPC)                            │
│                                                                                               │
│  State:                                                                                       │
│    mu_:             mutex                                                                    │
│    current_leader_: string   (cached leader address, e.g. "dscc-node-1:50051")              │
│    channels_:       map<string, grpc::Channel>  (connection cache, never recreated)          │
│                                                                                               │
│  Threads (1 background):                                                                      │
│  ┌──────────────────────────────────────────────────────────────────────────────────────────┐ │
│  │  LeaderPollLoop:                                                                        │ │
│  │    LOOP while running_:                                                                 │ │
│  │      RefreshLeader():                                                                   │ │
│  │        FOR EACH backend in backend_nodes_:                                              │ │
│  │          GetLeader RPC (750ms timeout) → dscc_raft.RaftService/GetLeader               │ │
│  │          if response.is_leader or response.leader_address non-empty:                    │ │
│  │            cache the leader address, return                                             │ │
│  │      sleep(100ms)                                                                       │ │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                               │
│  Request Forwarding (ForwardWithLeaderRetry<Req, Resp, RpcFn>):                              │
│  ┌──────────────────────────────────────────────────────────────────────────────────────────┐ │
│  │  target = RefreshLeader() or CurrentLeader()                                            │ │
│  │  if target empty → return UNAVAILABLE "No leader available"                             │ │
│  │                                                                                          │ │
│  │  FOR attempt = 0..2 (max 3 attempts):                                                  │ │
│  │    Create ClientContext with 35s deadline                                               │ │
│  │    Create stub from cached channel for target                                           │ │
│  │    Call rpc_fn(stub, context, request, response)                                        │ │
│  │                                                                                          │ │
│  │    if OK → cache target as leader, return OK                                            │ │
│  │                                                                                          │ │
│  │    Extract "leader-address" from gRPC trailing metadata                                 │ │
│  │                                                                                          │ │
│  │    if FAILED_PRECONDITION "NOT_LEADER":                                                 │ │
│  │      target = redirect address or RefreshLeader()                                       │ │
│  │      if found → continue                                                                │ │
│  │                                                                                          │ │
│  │    if UNAVAILABLE or DEADLINE_EXCEEDED:                                                 │ │
│  │      target = RefreshLeader()                                                           │ │
│  │      if found → continue                                                                │ │
│  │                                                                                          │ │
│  │    Otherwise → return the error directly                                                │ │
│  │                                                                                          │ │
│  │  All attempts exhausted → return UNAVAILABLE "Leader changed during forwarding"         │ │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                               │
└───────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 9. Qdrant Communication Layer

The Qdrant client is implemented via **raw POSIX TCP sockets** with hand-crafted HTTP/1.1 requests. There is no HTTP library dependency.

```
┌─── send_http_json(method, target, body) ──────────────────────────────────────────────────────┐
│                                                                                                │
│  1. DNS Resolution                                                                             │
│     getaddrinfo(qdrant_host_, qdrant_port_)  →  struct addrinfo*                              │
│                                                                                                │
│  2. Socket Creation & Connection                                                               │
│     Iterate addrinfo linked list:                                                             │
│       socket(family, SOCK_STREAM, protocol)                                                   │
│       connect(fd, addr, addrlen)                                                              │
│       break on first successful connect                                                       │
│                                                                                                │
│  3. Build HTTP/1.1 Request                                                                     │
│     ┌─────────────────────────────────────────────────┐                                       │
│     │  PUT /collections/dscc_memory_e2e/points?wait=true HTTP/1.1\r\n                        │
│     │  Host: qdrant:6333\r\n                                                                 │
│     │  Content-Type: application/json\r\n                                                    │
│     │  Connection: close\r\n                                                                 │
│     │  Content-Length: <N>\r\n                                                               │
│     │  \r\n                                                                                  │
│     │  {"points":[{"id":<fnv1a_hash>,"vector":[0.123,...,0.456],                            │
│     │    "payload":{"agent_id":"...","source_file":"...","timestamp_unix_ms":...             │
│     │     ,"raw_text":"..."}}]}                                                              │
│     └─────────────────────────────────────────────────┘                                       │
│                                                                                                │
│  4. Send (send_all loop until all bytes written)                                              │
│                                                                                                │
│  5. Receive (recv loop until connection closed, into 4KB buffer)                              │
│                                                                                                │
│  6. Parse HTTP status code from response first line                                           │
│     "HTTP/1.1 200 OK" → status_code = 200                                                    │
│                                                                                                │
│  7. Close socket                                                                               │
│                                                                                                │
│  Retry policy (upsert only):                                                                  │
│    3 attempts, backoff = 75ms × attempt_number                                                │
│    Retry on: send failure, HTTP 500 with "Please retry" in body                               │
│                                                                                                │
│  Collection auto-creation:                                                                     │
│    ensure_qdrant_collection(384)                                                              │
│    PUT /collections/dscc_memory_e2e  body: {"vectors":{"size":384,"distance":"Cosine"}}      │
│    Accepts: 200, 201, 409, or 400 with "already exists"                                      │
└────────────────────────────────────────────────────────────────────────────────────────────────┘

Point ID Generation (FNV-1a):
┌────────────────────────────────────────────────────────────────┐
│  hash = 1469598103934665603 (FNV offset basis)                │
│  FOR EACH byte c in agent_id:                                  │
│    hash ^= c                                                   │
│    hash *= 1099511628211 (FNV prime)                           │
│  mixed = (timestamp_unix_ms << 22) ^ (hash & ((1<<22) - 1))  │
│  point_id = mixed & INT64_MAX                                  │
└────────────────────────────────────────────────────────────────┘
```

---

## 10. Cosine Similarity Implementation

```
cosine_similarity(a[384], b[384]) → float:
    ┌─────────────────────────────────────────────────────────────────────┐
    │  if a.empty() || b.empty() || a.size() != b.size() → return 0.0   │
    │                                                                     │
    │  dot = 0.0    (double precision accumulation)                      │
    │  norm_a = 0.0                                                      │
    │  norm_b = 0.0                                                      │
    │                                                                     │
    │  FOR i = 0 .. 383:                                                 │
    │    dot    += double(a[i]) * double(b[i])                           │
    │    norm_a += double(a[i]) * double(a[i])                           │
    │    norm_b += double(b[i]) * double(b[i])                           │
    │                                                                     │
    │  if norm_a <= 0 or norm_b <= 0 → return 0.0                       │
    │                                                                     │
    │  similarity = dot / (sqrt(norm_a) * sqrt(norm_b))                  │
    │  clamp to [-1.0, 1.0]                                              │
    │  return float(similarity)                                          │
    │                                                                     │
    │  Complexity: O(d) where d=384 (embedding dimension)                │
    │  Conflict check: O(n×d) where n = active lock count               │
    └─────────────────────────────────────────────────────────────────────┘
```

---

## 11. gRPC Service Interfaces

### 11.1 Client-Facing — `dscc.proto`

```
┌─── LockService (dscc package) ─────────────────────────────────────────────────────────────┐
│                                                                                             │
│  ┌── Ping ──────────────────────────────────────────────────────────────────────────────┐   │
│  │  Request:   PingRequest  { from_node: string }                                      │   │
│  │  Response:  PingResponse { message: string }                                        │   │
│  │  Purpose:   Health check, returns "pong from <node_id> to <from_node>"              │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  ┌── AcquireGuard ──────────────────────────────────────────────────────────────────────┐   │
│  │  Request:   AcquireRequest                                                          │   │
│  │    ├── agent_id:          string                                                    │   │
│  │    ├── embedding:         repeated float  (384 values)                              │   │
│  │    ├── payload_text:      string          (natural language)                        │   │
│  │    ├── source_file:       string          (e.g. "A.json")                          │   │
│  │    ├── timestamp_unix_ms: int64           (client-provided or server-generated)     │   │
│  │    └── operation_type:    enum { UNSPECIFIED=0, WRITE=1, READ=2 }                  │   │
│  │                                                                                      │   │
│  │  Response:  AcquireResponse                                                         │   │
│  │    ├── granted:                    bool                                              │   │
│  │    ├── message:                    string                                           │   │
│  │    ├── server_received_unix_ms:    int64   ◄── when leader got the RPC             │   │
│  │    ├── lock_acquired_unix_ms:      int64   ◄── when semantic lock granted          │   │
│  │    ├── qdrant_write_complete_unix_ms: int64 ◄── when Qdrant op returned            │   │
│  │    ├── lock_released_unix_ms:      int64   ◄── when Raft RELEASE applied           │   │
│  │    ├── lock_wait_ms:               int64   ◄── queue wait duration                 │   │
│  │    ├── blocking_similarity_score:  float   ◄── cosine sim that caused block        │   │
│  │    ├── blocking_agent_id:          string  ◄── who blocked this request            │   │
│  │    ├── leader_redirect:            string  ◄── for NOT_LEADER redirects            │   │
│  │    ├── serving_node_id:            string  ◄── which node processed this           │   │
│  │    ├── wait_position:              int32   ◄── position in first queue             │   │
│  │    ├── wake_count:                 int32   ◄── cv signal count                     │   │
│  │    ├── queue_hops:                 int32   ◄── requeue count                       │   │
│  │    └── active_lock_count:          int32   ◄── locks held at time of grant         │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  ┌── ReleaseGuard ──────────────────────────────────────────────────────────────────────┐   │
│  │  Request:   ReleaseRequest  { agent_id: string }                                    │   │
│  │  Response:  ReleaseResponse { success: bool }                                       │   │
│  │  Purpose:   Standalone release (normally release is internal to AcquireGuard)       │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

### 11.2 Raft Internal — `dscc_raft.proto`

```
┌─── RaftService (dscc_raft package) ────────────────────────────────────────────────────────┐
│                                                                                             │
│  ┌── RequestVote ───────────────────────────────────────────────────────────────────────┐   │
│  │  Request:   VoteRequest                                                             │   │
│  │    ├── term:            int64                                                       │   │
│  │    ├── candidate_id:    string                                                      │   │
│  │    ├── last_log_index:  int64                                                       │   │
│  │    └── last_log_term:   int64                                                       │   │
│  │  Response:  VoteResponse  { term: int64, vote_granted: bool }                      │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  ┌── AppendEntries ─────────────────────────────────────────────────────────────────────┐   │
│  │  Request:   AppendRequest                                                           │   │
│  │    ├── term:                    int64                                                │   │
│  │    ├── leader_id:               string                                              │   │
│  │    ├── leader_service_address:  string   ◄── client-facing port for redirects      │   │
│  │    ├── prev_log_index:          int64                                               │   │
│  │    ├── prev_log_term:           int64                                               │   │
│  │    ├── entries:                 repeated LogEntry                                   │   │
│  │    └── leader_commit:           int64                                               │   │
│  │  Response:  AppendResponse                                                          │   │
│  │    ├── term:           int64                                                        │   │
│  │    ├── success:        bool                                                         │   │
│  │    ├── match_index:    int64                                                        │   │
│  │    ├── conflict_term:  int64   ◄── for fast rollback on log inconsistency          │   │
│  │    └── conflict_index: int64   ◄── hint: first index of conflict_term              │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  ┌── LogEntry ──────────────────────────────────────────────────────────────────────────┐   │
│  │  ├── term:      int64                                                                │   │
│  │  ├── op_type:   enum { ACQUIRE=0, RELEASE=1 }                                       │   │
│  │  ├── agent_id:  string                                                               │   │
│  │  ├── embedding: repeated float  (384 values, only for ACQUIRE)                      │   │
│  │  └── theta:     float           (only for ACQUIRE)                                  │   │
│  │  Wire size: ~1,560 bytes for ACQUIRE, ~20 bytes for RELEASE                         │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  ┌── GetLeader ─────────────────────────────────────────────────────────────────────────┐   │
│  │  Request:   LeaderQuery  (empty message)                                            │   │
│  │  Response:  LeaderInfo                                                              │   │
│  │    ├── leader_id:       string                                                      │   │
│  │    ├── leader_address:  string  (client-facing host:port)                          │   │
│  │    ├── current_term:    int64                                                       │   │
│  │    └── is_leader:       bool    (true if this node is the leader)                  │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  ┌── InstallSnapshot (stub, not fully implemented) ─────────────────────────────────────┐   │
│  │  Request:  SnapshotRequest { term, leader_id, last_included_index/term, data }      │   │
│  │  Response: SnapshotResponse { term }                                                │   │
│  └──────────────────────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 12. Thread Model Per Process

### dscc-node (6+ threads)

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  Thread 1:  main thread                                                                    │
│             grpc::Server::Wait() — blocked, handles incoming RPCs via gRPC thread pool     │
│                                                                                            │
│  Thread 2:  gRPC thread pool (managed by gRPC)                                            │
│             Handles concurrent AcquireGuard, ReleaseGuard, Ping RPCs                      │
│             Each AcquireGuard may block on lock_table_->acquire() cv.wait                  │
│                                                                                            │
│  Thread 3:  RaftNode::ElectionTimerLoop                                                   │
│             Checks election_deadline_ every 10ms, starts election if expired              │
│                                                                                            │
│  Thread 4:  RaftNode::HeartbeatLoop                                                       │
│             Leader: spawns N short-lived threads per heartbeat round                       │
│             Sleeps 75ms between rounds                                                     │
│                                                                                            │
│  Thread 5:  RaftNode::ApplyLoop                                                           │
│             Waits on apply_cv_, applies committed entries via on_commit_ callback          │
│             Calls lock_table_->apply_acquire() or apply_release()                         │
│                                                                                            │
│  Thread 6+: Ephemeral replication threads (spawned by Propose and HeartbeatLoop)           │
│             One per peer per replication round                                              │
│             ReplicateToFollower() — sends AppendEntries, updates match/next indices        │
│                                                                                            │
│  Synchronization:                                                                          │
│    RaftNode::mu_         — protects all Raft state (term, log, indices, state)             │
│    RaftNode::commit_cv_  — notifies Propose when commit_index advances                    │
│    RaftNode::apply_cv_   — notifies ApplyLoop and WaitUntilApplied                        │
│    ActiveLockTable::mu_  — protects active_ map and all waiter queues                     │
│    WaitQueueEntry::cv    — per-waiter wakeup (shared_ptr, one per blocked request)        │
│    g_log_mu              — serializes console output across all threads                   │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

### dscc-proxy (2+ threads)

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  Thread 1:  main thread                                                                    │
│             grpc::Server::Wait()                                                           │
│                                                                                            │
│  Thread 2:  ProxyServiceImpl::LeaderPollLoop                                              │
│             Polls GetLeader every 100ms across all 5 backends                             │
│                                                                                            │
│  Thread 3+: gRPC thread pool                                                              │
│             Handles forwarded AcquireGuard/ReleaseGuard RPCs                              │
│                                                                                            │
│  Synchronization:                                                                          │
│    ProxyServiceImpl::mu_ — protects current_leader_ and channels_ map                    │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 13. Environment Variable Configuration Reference

### dscc-node

```
┌─────────────────────────────────┬──────────┬──────────────────────────────────────────────┐
│  Variable                       │ Default  │ Purpose                                       │
├─────────────────────────────────┼──────────┼──────────────────────────────────────────────┤
│  NODE_ID                        │ "node-1" │ Unique identifier for this Raft node          │
│  PORT                           │ "50051"  │ Client-facing gRPC port (LockService)         │
│  RAFT_PORT                      │ "50061"  │ Inter-node gRPC port (RaftService)             │
│  ADVERTISE_HOST                 │ "0.0.0.0"│ Hostname for service_address in Raft metadata │
│  PEERS                          │ ""       │ CSV of peer Raft addresses                     │
│  THETA                          │ 0.85     │ Cosine similarity conflict threshold           │
│  LOCK_HOLD_MS                   │ 0        │ Artificial hold time after Qdrant write        │
│  RAFT_PROPOSE_TIMEOUT_MS        │ 5000     │ Max time to wait for quorum commit             │
│  HEARTBEAT_MS                   │ 75       │ Raft leader heartbeat interval                 │
│  ELECTION_TIMEOUT_MIN_MS        │ 600      │ Randomized election timeout lower bound        │
│  ELECTION_TIMEOUT_MAX_MS        │ 1000     │ Randomized election timeout upper bound        │
│  RAFT_RPC_TIMEOUT_MS            │ 150      │ Per-RPC deadline for AppendEntries/RequestVote │
│  QDRANT_HOST                    │ "qdrant" │ Qdrant server hostname                         │
│  QDRANT_PORT                    │ "6333"   │ Qdrant HTTP port                               │
│  QDRANT_COLLECTION              │ "dscc_memory" │ Qdrant collection name                   │
└─────────────────────────────────┴──────────┴──────────────────────────────────────────────┘
```

### dscc-proxy

```
┌─────────────────────────────────┬──────────┬──────────────────────────────────────────────┐
│  Variable                       │ Default  │ Purpose                                       │
├─────────────────────────────────┼──────────┼──────────────────────────────────────────────┤
│  PROXY_PORT                     │ "50050"  │ Proxy listening port                          │
│  BACKEND_NODES                  │ ""       │ CSV of backend node client-facing addresses   │
│  LEADER_POLL_MS                 │ 100      │ How often to poll GetLeader across backends   │
│  REQUEST_TIMEOUT_MS             │ 35000    │ Deadline for forwarded RPCs                   │
│  LEADER_RPC_TIMEOUT_MS          │ 750      │ Deadline for GetLeader poll RPCs              │
└─────────────────────────────────┴──────────┴──────────────────────────────────────────────┘
```

---

## 14. Build Targets and Dependencies

```
┌─── CMake Build Graph ─────────────────────────────────────────────────────────────────────────┐
│                                                                                                │
│  dscc_proto (STATIC library)                                                                  │
│    ├── generated from: proto/dscc.proto, proto/dscc_raft.proto                                │
│    ├── protoc → .pb.cc/.pb.h                                                                 │
│    ├── protoc + grpc_cpp_plugin → .grpc.pb.cc/.grpc.pb.h                                     │
│    └── links: gRPC::grpc++, protobuf::libprotobuf                                            │
│                                                                                                │
│  dscc-node (EXECUTABLE)           ◄── The distributed node binary                            │
│    ├── src/main.cpp                                                                            │
│    ├── src/active_lock_table.cpp                                                               │
│    ├── src/lock_service_impl.cpp                                                               │
│    ├── src/raft_node.cpp                                                                       │
│    ├── src/raft_service_impl.cpp                                                               │
│    ├── src/threadsafe_log.cpp                                                                  │
│    └── links: dscc_proto, gRPC, protobuf, Threads                                            │
│                                                                                                │
│  dscc-proxy (EXECUTABLE)          ◄── The leader-aware proxy                                  │
│    ├── src/proxy_main.cpp                                                                      │
│    ├── src/proxy_service_impl.cpp                                                              │
│    └── links: dscc_proto, gRPC, protobuf, Threads                                            │
│                                                                                                │
│  dscc-testbench (EXECUTABLE)      ◄── In-process lock table tests (no Docker/gRPC)           │
│    ├── src/testbench.cpp                                                                       │
│    ├── src/active_lock_table.cpp                                                               │
│    ├── src/threadsafe_log.cpp                                                                  │
│    └── links: Threads                                                                         │
│                                                                                                │
│  dscc-e2e-bench (EXECUTABLE)      ◄── E2E demo harness (C++20, std::barrier)                 │
│    ├── src/e2e_bench.cpp                                                                       │
│    ├── src/threadsafe_log.cpp                                                                  │
│    └── links: dscc_proto, gRPC, protobuf, Threads                                            │
│                                                                                                │
│  dscc-benchmark (EXECUTABLE)      ◄── Curated 10-scenario benchmark (C++20)                  │
│    ├── src/benchmark_runner.cpp                                                                │
│    └── links: dscc_proto, gRPC, protobuf, Threads                                            │
│                                                                                                │
│  dscc-raft-test (EXECUTABLE)      ◄── In-process 3-node Raft test                            │
│    ├── src/raft_test.cpp                                                                       │
│    ├── src/raft_node.cpp                                                                       │
│    ├── src/raft_service_impl.cpp                                                               │
│    ├── src/threadsafe_log.cpp                                                                  │
│    └── links: dscc_proto, gRPC, protobuf, Threads                                            │
│                                                                                                │
│  Compiler: C++17 (core), C++20 (benchmarks — uses std::barrier)                              │
│  Container: Ubuntu 22.04, build-essential, cmake, protobuf-dev, grpc++-dev                   │
└────────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 15. Docker Compose Service Topology

```
┌─── docker-compose.yml ─────────────────────────────────────────────────────────────────────────┐
│                                                                                                 │
│  services:                                                                                      │
│                                                                                                 │
│  ┌─ qdrant ───────────────────────┐   Image: qdrant/qdrant                                    │
│  │  Port: 6333:6333               │   No depends_on                                           │
│  └────────────────────────────────┘                                                            │
│                                                                                                 │
│  ┌─ embedding-service ────────────┐   Image: ollama/ollama:latest                             │
│  │  Port: 7997:11434             │   Volumes: ./.cache/ollama:/root/.ollama                   │
│  └────────────────────────────────┘                                                            │
│                                                                                                 │
│  ┌─ dscc-node ────────────────────┐   Build: docker/Dockerfile                                │
│  │  Port: 50051:50051             │   Env: PORT, THETA, LOCK_HOLD_MS,                         │
│  │  depends_on:                   │        QDRANT_HOST, QDRANT_PORT,                          │
│  │    - qdrant                    │        QDRANT_COLLECTION                                  │
│  │    - embedding-service         │                                                            │
│  └────────────────────────────────┘                                                            │
│                                                                                                 │
│  NOTE: The 5-node + proxy topology is configured by the benchmark runner                      │
│  via programmatic docker-compose override or direct Docker commands.                           │
│  The checked-in docker-compose.yml shows the single-node configuration.                       │
│                                                                                                 │
│  Full distributed topology (configured by e2e_bench/benchmark_runner):                        │
│  ┌──────────────────────────────────────────────────────────────────────────────────────────┐  │
│  │  dscc-node-1  PORT=50051  RAFT_PORT=50061  PEERS=node-2:50062,...,node-5:50065         │  │
│  │  dscc-node-2  PORT=50052  RAFT_PORT=50062  PEERS=node-1:50061,...,node-5:50065         │  │
│  │  dscc-node-3  PORT=50053  RAFT_PORT=50063  PEERS=node-1:50061,...,node-4:50064,5:50065│  │
│  │  dscc-node-4  PORT=50054  RAFT_PORT=50064  PEERS=...                                  │  │
│  │  dscc-node-5  PORT=50055  RAFT_PORT=50065  PEERS=...                                  │  │
│  │  dscc-proxy   PROXY_PORT=50050  BACKEND_NODES=node-1:50051,...,node-5:50055           │  │
│  └──────────────────────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 16. End-to-End Request Flow (Distributed, Write Path)

```
Time ──────────────────────────────────────────────────────────────────────────────────────────▶

Benchmark                    Proxy                       Leader (node-3)              Followers           Qdrant
    │                          │                              │                           │                 │
    │  AcquireGuard(           │                              │                           │                 │
    │    agent_id="agt_A",     │                              │                           │                 │
    │    embedding=[384],      │                              │                           │                 │
    │    payload="Review...",  │                              │                           │                 │
    │    op=WRITE)             │                              │                           │                 │
    │─────────gRPC:50050──────▶│                              │                           │                 │
    │                          │  RefreshLeader()             │                           │                 │
    │                          │──GetLeader──▶node-1──────────│                           │                 │
    │                          │  ◀──{leader=node-3:50053}────│                           │                 │
    │                          │                              │                           │                 │
    │                          │──AcquireGuard─gRPC:50053────▶│                           │                 │
    │                          │                              │                           │                 │
    │                          │                              │ ① Validate inputs         │                 │
    │                          │                              │ ② IsLeader? YES           │                 │
    │                          │                              │ ③ lock_table_.acquire()   │                 │
    │                          │                              │    find_conflict_locked()  │                 │
    │                          │                              │    O(n) scan active locks  │                 │
    │                          │                              │    cos_sim >= θ?           │                 │
    │                          │                              │    NO → insert into active_│                 │
    │                          │                              │    (or YES → cv.wait...)   │                 │
    │                          │                              │                           │                 │
    │                          │                              │ ④ Raft Propose(ACQUIRE)   │                 │
    │                          │                              │───AppendEntries──────────▶│ node-1          │
    │                          │                              │───AppendEntries──────────▶│ node-2          │
    │                          │                              │───AppendEntries──────────▶│ node-4          │
    │                          │                              │───AppendEntries──────────▶│ node-5          │
    │                          │                              │◀──{success, match=N}──────│ (3 of 4 = ok)   │
    │                          │                              │   AdvanceCommitIndex()    │                 │
    │                          │                              │   commit_index_ = N       │                 │
    │                          │                              │   ApplyLoop: on_commit_   │                 │
    │                          │                              │     apply_acquire(agt_A)  │                 │
    │                          │                              │                           │                 │
    │                          │                              │ ⑤ Qdrant upsert           │                 │
    │                          │                              │───PUT /points?wait=true───────────────────▶│
    │                          │                              │◀──HTTP 200────────────────────────────────│
    │                          │                              │                           │                 │
    │                          │                              │ ⑥ sleep(lock_hold_ms)     │                 │
    │                          │                              │                           │                 │
    │                          │                              │ ⑦ Raft Propose(RELEASE)   │                 │
    │                          │                              │───AppendEntries──────────▶│                 │
    │                          │                              │◀──{success}───────────────│                 │
    │                          │                              │   WaitUntilApplied()      │                 │
    │                          │                              │   ApplyLoop: on_commit_   │                 │
    │                          │                              │     apply_release(agt_A)  │                 │
    │                          │                              │     rebalance_waiters()   │                 │
    │                          │                              │     notify granted waiters│                 │
    │                          │                              │                           │                 │
    │                          │◀──AcquireResponse{           │                           │                 │
    │                          │    granted=true,             │                           │                 │
    │                          │    lock_wait_ms,             │                           │                 │
    │                          │    blocking_sim,             │                           │                 │
    │                          │    queue_hops, ...}          │                           │                 │
    │◀────────────────────────│                              │                           │                 │
    │                          │                              │                           │                 │
```

---

## 17. Conflict Detection Scenario Visualization

```
Active Lock Table State:
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│  active_ = {                                                                           │
│    "sustainability_agent_0": { centroid=[...], θ=0.78, waiters=[] }                   │
│    "construction_agent_0":   { centroid=[...], θ=0.78, waiters=["client_agent_0"] }   │
│  }                                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────────┘

Incoming: safety_agent_0, embedding=[...], θ=0.78

find_conflict_locked:
  vs sustainability_agent_0: cos_sim = 0.87 → 0.87 >= 0.78 → CONFLICT (candidate)
  vs construction_agent_0:   cos_sim = 0.45 → 0.45 <  0.78 → no conflict

  Best conflict: sustainability_agent_0, similarity=0.87

Result: safety_agent_0 is queued behind sustainability_agent_0

┌─────────────────────────────────────────────────────────────────────────────────────────┐
│  active_ = {                                                                           │
│    "sustainability_agent_0": { centroid=[...], θ=0.78,                                │
│       waiters=[                                                                        │
│         WaitQueueEntry{ agent="safety_agent_0", cv=..., queue_hops=0 }                │
│       ]                                                                                │
│    }                                                                                   │
│    "construction_agent_0":   { centroid=[...], θ=0.78,                                │
│       waiters=[                                                                        │
│         WaitQueueEntry{ agent="client_agent_0", cv=..., queue_hops=0 }                │
│       ]                                                                                │
│    }                                                                                   │
│  }                                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────────┘

When sustainability_agent_0 releases:
  rebalance_waiters_locked:
    safety_agent_0: find_conflict against remaining active locks
      vs construction_agent_0: cos_sim = 0.35 → no conflict
    → GRANT: safety_agent_0 inserted into active_, cv.notify()

If instead cos_sim vs construction was 0.82:
    → REQUEUE: safety_agent_0 moved to construction_agent_0's waiters, queue_hops=1
```

---

## 18. Timing Constants Summary

```
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│                                                                                         │
│  RAFT TIMING                                                                           │
│  ├── Heartbeat interval:              75 ms                                            │
│  ├── Election timeout range:          [600, 1000] ms    (ratio: 8–13× heartbeat)      │
│  ├── Per-RPC timeout (AppendEntries): 150 ms                                          │
│  ├── Propose timeout (quorum wait):   5,000 ms                                        │
│  ├── Election timer poll:             10 ms                                            │
│  └── Commit wait poll:                25 ms                                            │
│                                                                                         │
│  PROXY TIMING                                                                          │
│  ├── Leader poll interval:            100 ms                                           │
│  ├── Per-forwarded-RPC deadline:      35,000 ms                                       │
│  ├── GetLeader RPC timeout:           750 ms                                          │
│  └── Max retry attempts:             3                                                 │
│                                                                                         │
│  LOCK / QDRANT TIMING                                                                  │
│  ├── Lock hold (write, configurable): 750 ms (default for benchmarks)                 │
│  ├── Qdrant upsert retry backoff:     75ms × attempt (max 3 attempts)                 │
│  └── Qdrant read: single attempt, no retry                                            │
│                                                                                         │
│  QUORUM                                                                                │
│  ├── Total nodes:                     5                                                │
│  ├── Quorum size:                     3    ((5+1)/2 = 3)                              │
│  └── Tolerated failures:             2                                                 │
│                                                                                         │
└─────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 19. Known Architectural Caveats

```
┌─────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                                                             │
│  CAVEAT 1: Leader-Admitted-Then-Replicated                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  Current:  acquire() → Propose(ACQUIRE) → Qdrant → Propose(RELEASE)              │   │
│  │  Correct:  Propose(ACQUIRE) → apply → acquire() → Qdrant → Propose(RELEASE)      │   │
│  │                                                                                     │   │
│  │  Risk window: leader crash between acquire() and Propose(ACQUIRE) commit           │   │
│  │    → lock held locally but never replicated                                        │   │
│  │    → new leader has no knowledge of it                                             │   │
│  │    → if Qdrant write also didn't happen: harmless                                  │   │
│  │    → if Qdrant write DID happen: orphaned write in vector DB                      │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  CAVEAT 2: No Lease/TTL Expiration                                                        │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  If an agent disconnects between ACQUIRE commit and RELEASE,                       │   │
│  │  the lock is held indefinitely on all nodes via Raft replay.                       │   │
│  │  No automatic cleanup mechanism exists.                                            │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  CAVEAT 3: In-Memory Raft Log (No WAL)                                                    │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  log_ is a std::vector in memory. No write-ahead log to disk.                     │   │
│  │  Simultaneous restart of all 5 nodes → total state loss.                          │   │
│  │  Individual restarts: the rejoining node replays from the leader's in-memory log.  │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  CAVEAT 4: Raw Socket Qdrant Client                                                       │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  Hand-crafted HTTP/1.1 over POSIX sockets: no connection pooling, no keep-alive,   │   │
│  │  no chunked transfer, no TLS. New TCP connection per request.                      │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  CAVEAT 5: O(n) Conflict Detection                                                        │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  Every acquire scans all active locks: O(n × 384) per request.                    │   │
│  │  Acceptable for current scale (< 20 concurrent locks).                             │   │
│  │  Would need spatial indexing (VP-tree, ball tree) for 100+ active locks.           │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
│  CAVEAT 6: Sequential Election Vote Solicitation                                           │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  StartElection() sends RequestVote to peers sequentially, not in parallel.         │   │
│  │  With 150ms RPC timeout and 4 peers, worst case: 600ms just for vote collection.   │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                             │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 20. File-to-Component Map

```
┌────────────────────────────────┬─────────────────────────────────────────────────────────────┐
│  File                          │  Component / Responsibility                                 │
├────────────────────────────────┼─────────────────────────────────────────────────────────────┤
│  src/main.cpp                  │  dscc-node entry point, wires all components                │
│  src/proxy_main.cpp            │  dscc-proxy entry point                                    │
│  src/active_lock_table.h       │  SemanticLock, WaitQueueEntry, AcquireTrace structs        │
│  src/active_lock_table.cpp     │  Lock table: acquire, release, rebalance, cosine_sim       │
│  src/lock_service_impl.h       │  LockServiceImpl declaration                               │
│  src/lock_service_impl.cpp     │  AcquireGuard critical path, Qdrant I/O, FNV-1a hashing   │
│  src/raft_node.h               │  RaftNode, RaftConfig, RaftState, ApplyCallback            │
│  src/raft_node.cpp             │  Raft FSM, election, heartbeat, replication, apply loop    │
│  src/raft_service_impl.h/.cpp  │  gRPC wrapper for Raft RPCs (thin delegation layer)        │
│  src/proxy_service_impl.h/.cpp │  Proxy: leader polling, forwarding, retry logic            │
│  src/threadsafe_log.h/.cpp     │  Global mutex-guarded log_line() for console output        │
│  proto/dscc.proto              │  LockService interface + AcquireRequest/Response           │
│  proto/dscc_raft.proto         │  RaftService interface + LogEntry + AppendRequest/Response  │
│  src/testbench.cpp             │  In-process lock table unit tests                          │
│  src/e2e_bench.cpp             │  E2E demo harness (7 scenarios, Docker orchestration)      │
│  src/benchmark_runner.cpp      │  Curated benchmark (10 scenarios, correctness validation)  │
│  src/raft_test.cpp             │  In-process 3-node Raft test                               │
│  docker/Dockerfile             │  Ubuntu 22.04 + deps, builds dscc-node and dscc-proxy      │
│  docker-compose.yml            │  Service definitions (Qdrant, Ollama, dscc-node)           │
│  CMakeLists.txt                │  Build system: 6 targets, proto generation                 │
│  demo_inputs/A-E.json          │  Agent persona definitions + payload schedules             │
└────────────────────────────────┴─────────────────────────────────────────────────────────────┘
```
