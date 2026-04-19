# Raft Implementation — Complete Technical Reference

This document is the single source of truth for how the Raft consensus layer works
in the DSCC system.  It covers every code path, every failure scenario, every log
message, and every known limitation.  **Update this file whenever the Raft or lock
service code changes.**

Last updated: 2026-04-18 (branch `fix-raft-logic`)

---

## Table of Contents

1. [File Map](#1-file-map)
2. [Data Structures and State](#2-data-structures-and-state)
3. [Protobuf Schema](#3-protobuf-schema)
4. [Background Threads](#4-background-threads)
5. [Leader Election](#5-leader-election)
6. [Log Replication](#6-log-replication)
7. [Commit and Apply](#7-commit-and-apply)
8. [The on_commit\_ Callback and Lock Table Integration](#8-the-on_commit_-callback-and-lock-table-integration)
9. [The AcquireGuard Request Lifecycle (Three Phases)](#9-the-acquireguard-request-lifecycle-three-phases)
10. [The ReleaseGuard Request Lifecycle](#10-the-releaseguard-request-lifecycle)
11. [Compensating RELEASE — Ghost Lock Prevention](#11-compensating-release--ghost-lock-prevention)
12. [Pending Lock Semantics](#12-pending-lock-semantics)
13. [Failure Scenarios — Exhaustive Catalog](#13-failure-scenarios--exhaustive-catalog)
14. [Log Messages Reference](#14-log-messages-reference)
15. [Configuration Reference](#15-configuration-reference)
16. [Known Limitations and Open Issues](#16-known-limitations-and-open-issues)
17. [In-Process Raft Regression Testbench (`dscc-raft-test`)](#17-in-process-raft-regression-testbench-dscc-raft-test)

---

## 1. File Map

| File | Role |
|------|------|
| `proto/dscc_raft.proto` | Protobuf/gRPC schema for inter-node Raft RPCs |
| `proto/dscc.proto` | Protobuf/gRPC schema for client-facing lock RPCs |
| `src/raft_node.h` | `RaftNode` class declaration, `RaftConfig`, `RaftState` enum |
| `src/raft_node.cpp` | All Raft logic: election, replication, commit, apply |
| `src/raft_service_impl.h/cpp` | Thin gRPC wrapper — delegates to `RaftNode` methods |
| `src/active_lock_table.h` | `ActiveLockTable`, `SemanticLock`, `WaitQueueEntry` declarations |
| `src/active_lock_table.cpp` | Lock table logic: admission, conflict, release, rebalance |
| `src/lock_service_impl.h/cpp` | `LockServiceImpl` — bridges Raft + lock table + Qdrant |
| `src/main.cpp` | Wires everything together, reads env config, starts gRPC server |
| `src/raft_test.cpp` | In-process 3-node Raft regression suite (`dscc-raft-test`; see §17) |

---

## 2. Data Structures and State

### 2.1 RaftNode (src/raft_node.h)

```
class RaftNode
├── node_id_              string       unique identifier (e.g. "node-1")
├── service_address_      string       advertised client-facing address (e.g. "dscc-node-1:50051")
├── peer_addresses_       vector       Raft RPC addresses of all other nodes
├── on_commit_            function     callback invoked for each committed log entry
├── config_               RaftConfig   timing parameters
│
├── mu_                   mutex        protects ALL mutable state below
├── commit_cv_            cond_var     notifies Propose when commit_index_ advances
├── apply_cv_             cond_var     notifies ApplyLoop and WaitUntilApplied
│
├── current_term_         int64        monotonically increasing Raft term (starts at 0)
├── voted_for_            string       candidate this node voted for in current_term_ (empty = none)
├── log_                  vector       in-memory Raft log; index 0 is a sentinel (term=0)
│
├── commit_index_         int64        highest log index known to be committed (quorum)
├── last_applied_         int64        highest log index that on_commit_ has been called for
├── state_                RaftState    FOLLOWER | CANDIDATE | LEADER
├── current_leader_id_    string       node_id of the current leader
├── current_leader_address_ string     service_address of the current leader
│
├── next_index_           map          per-peer: next log index to send (leader only)
├── match_index_          map          per-peer: highest log index known replicated (leader only)
├── peers_                map          per-peer: gRPC channel
│
├── election_deadline_    time_point   when the election timer fires next
├── running_              atomic<bool> controls all background threads
└── 3 background threads  (see §4)
```

### 2.2 SemanticLock (src/active_lock_table.h)

```
struct SemanticLock
├── agent_id      string           which agent holds this lock
├── centroid      vector<float>    384-dim embedding used for conflict detection
├── threshold     float            cosine similarity threshold (θ)
├── pending       bool             true = reservation only, not yet Raft-committed
└── waiters       deque<WaitQueueEntry>   per-lock FIFO queue of blocked requests
```

### 2.3 LogEntry (proto/dscc_raft.proto)

```
message LogEntry
├── term       int64       Raft term when this entry was created
├── op_type    OpType      ACQUIRE (0) or RELEASE (1)
├── agent_id   string      which agent this entry is for
├── embedding  float[]     384-dim vector (only meaningful for ACQUIRE)
└── theta      float       similarity threshold (only meaningful for ACQUIRE)
```

---

## 3. Protobuf Schema

### 3.1 dscc_raft.proto — Inter-Node RPCs

```
service RaftService {
  rpc RequestVote(VoteRequest) returns (VoteResponse);
  rpc AppendEntries(AppendRequest) returns (AppendResponse);
  rpc InstallSnapshot(SnapshotRequest) returns (SnapshotResponse);
  rpc GetLeader(LeaderQuery) returns (LeaderInfo);
}
```

**RequestVote** — used during elections.
- Request: `{term, candidate_id, last_log_index, last_log_term}`
- Response: `{term, vote_granted}`

**AppendEntries** — used for heartbeats (empty entries) and log replication.
- Request: `{term, leader_id, leader_service_address, prev_log_index, prev_log_term, entries[], leader_commit}`
- Response: `{term, success, match_index, conflict_term, conflict_index}`

**InstallSnapshot** — stub; currently only updates term.
- Request: `{term, leader_id, last_included_index, last_included_term, data}`
- Response: `{term}`

**GetLeader** — used by the proxy to discover the current leader.
- Response: `{leader_id, leader_address, current_term, is_leader}`

### 3.2 dscc.proto — Client-Facing RPCs

```
service LockService {
  rpc Ping(PingRequest) returns (PingResponse);
  rpc AcquireGuard(AcquireRequest) returns (AcquireResponse);
  rpc ReleaseGuard(ReleaseRequest) returns (ReleaseResponse);
}
```

---

## 4. Background Threads

`RaftNode::Start()` spawns three threads.  All three run until `running_` becomes
`false` (via `Stop()`).

### 4.1 ElectionTimerLoop

```
every 10ms:
    lock(mu_)
    if state_ != LEADER and now >= election_deadline_:
        set should_start_election = true
    unlock(mu_)
    if should_start_election:
        StartElection()
```

Polls `election_deadline_` at 10ms granularity.  Only fires if the node is NOT a
leader and the deadline has passed (no heartbeat received in time).

### 4.2 HeartbeatLoop

```
every config_.heartbeat_ms (default 75ms):
    lock(mu_)
    if state_ == LEADER:
        peers_to_ping = peer_addresses_
    unlock(mu_)
    spawn one thread per peer → ReplicateToFollower(peer)
    join all threads
    sleep(heartbeat_ms)
```

The heartbeat sends `AppendEntries` which serves as both heartbeat (empty) and
data replication (with entries).  Each peer gets its own thread per heartbeat
cycle.  This also means unreplicated entries (e.g. from `AppendLocalEntry`) get
picked up automatically on the next heartbeat cycle.

### 4.3 ApplyLoop

```
loop:
    lock(mu_)
    wait on apply_cv_ until (last_applied_ < commit_index_) or !running_
    if !running_: return
    ++last_applied_
    entry = log_[last_applied_]
    unlock(mu_)

    on_commit_(entry)        // calls into lock table

    lock(mu_)
    apply_cv_.notify_all()   // wakes WaitUntilApplied callers
    unlock(mu_)
```

Processes committed entries **one at a time, in order**.  The `on_commit_`
callback runs **outside** `mu_` so it can safely acquire the lock table's own
mutex.  After each apply, `apply_cv_` is notified so `WaitUntilApplied` callers
can check whether their index has been reached.

---

## 5. Leader Election

### 5.1 StartElection()

Triggered by `ElectionTimerLoop` when `election_deadline_` expires.

```
lock(mu_)
    if state_ == LEADER: return (leaders don't re-elect)
    state_ = CANDIDATE
    ++current_term_
    voted_for_ = self
    clear leader info
    ResetElectionDeadlineLocked()
    capture election_term, last_log_index, last_log_term
unlock(mu_)

LOG: "[RAFT node-X] starting election term=<election_term>"

votes = 1 (self-vote)
FOR EACH peer (SEQUENTIALLY, not in parallel):
    SendRequestVote(peer, {term, candidate_id, last_log_index, last_log_term})
    timeout: config_.rpc_timeout_ms (150ms)

    if RPC fails: skip this peer
    lock(mu_)
        if response.term > current_term_:
            BecomeFollowerLocked(response.term)
            return  // someone else has a higher term
        if state_ != CANDIDATE or term changed: return  // pre-empted
        if vote_granted: ++votes
        if votes >= QuorumSize():
            BecomeLeaderLocked()
            break
    unlock(mu_)
```

**Key observations:**
- Votes are collected **sequentially** (not in parallel).  This simplifies the
  code but means election time is `O(N × rpc_timeout)` in the worst case.
- `QuorumSize() = (peer_count + 1) / 2 + 1`.  For a 5-node cluster: quorum = 3.
  For a 3-node cluster: quorum = 2.
- The election deadline is reset at the start, so if the election takes longer
  than the timeout, the node will start a new election with a higher term.

### 5.2 HandleRequestVote()

```
lock(mu_)
if request.term < current_term_:
    reject (stale term)
if request.term > current_term_:
    BecomeFollowerLocked(request.term)

can_vote = (voted_for_ is empty) or (voted_for_ == candidate_id)
up_to_date = IsLogUpToDateLocked(request.last_log_index, request.last_log_term)

if can_vote AND up_to_date:
    voted_for_ = candidate_id
    ResetElectionDeadlineLocked()
    grant vote
else:
    reject
```

**IsLogUpToDateLocked** — the Raft log-completeness check:
- Compare last log terms first.  Higher term wins.
- If terms are equal, compare last log indices.  Longer (or equal) log wins.

### 5.3 BecomeLeaderLocked()

```
state_ = LEADER
current_leader_id_ = self
current_leader_address_ = service_address_
FOR EACH peer:
    next_index_[peer] = log_.size()      // optimistic: assume peer is caught up
    match_index_[peer] = 0               // pessimistic: no confirmation yet
LOG: "[RAFT node-X] became leader term=<current_term_>"
```

### 5.4 BecomeFollowerLocked()

```
state_ = FOLLOWER
current_term_ = max(current_term_, term)
voted_for_ = ""                          // clear vote for new term
current_leader_id_ = leader_id
current_leader_address_ = leader_address
ResetElectionDeadlineLocked()
```

### 5.5 Single-Node Mode

If `peer_addresses_` is empty (no peers), `Start()` immediately makes the node
leader in term 1.  `Propose` and `AppendLocalEntry` commit instantly by advancing
`commit_index_` directly.

---

## 6. Log Replication

### 6.1 ReplicateToFollower(peer_address)

Called from `HeartbeatLoop` and from `Propose`.  Runs in a **retry loop** that
handles log inconsistency:

```
WHILE running_:
    lock(mu_)
    if not LEADER: return false

    next_index = max(1, next_index_[peer])
    build AppendRequest:
        term = current_term_
        leader_id = node_id_
        leader_service_address = service_address_
        prev_log_index = next_index - 1
        prev_log_term = log_[prev_log_index].term()
        leader_commit = commit_index_
        entries = log_[next_index .. end]     // all entries the peer hasn't seen
    sent_term = current_term_
    unlock(mu_)

    send AppendEntries RPC (timeout: config_.rpc_timeout_ms = 150ms)

    if RPC fails (network error, timeout):
        return false   // will retry on next heartbeat cycle

    lock(mu_)
    if response.term > current_term_:
        BecomeFollowerLocked(response.term)
        return false
    if no longer LEADER or term changed:
        return false

    if response.success:
        match_index_[peer] = max(match_index_[peer], response.match_index)
        next_index_[peer] = match_index_[peer] + 1
        AdvanceCommitIndexLocked()
        return true

    // Log inconsistency — follower rejected because prev_log check failed
    retry_index = next_index_[peer] - 1
    if response.conflict_index > 0:
        retry_index = response.conflict_index    // fast rollback
    next_index_[peer] = max(1, retry_index)
    unlock(mu_)
    // loop continues — will retry with the decremented next_index
```

**Key observations:**
- The inner retry loop handles log inconsistency within a single call.  If the
  follower's log diverges, the leader decrements `next_index` and retries
  immediately (without waiting for the next heartbeat).
- `entries` includes ALL entries from `next_index` to the end of the log.  There
  is no batching limit — large logs can produce large RPCs.
- Each `ReplicateToFollower` call creates a **new gRPC stub** from the cached
  channel.  The channel itself is reused.

### 6.2 HandleAppendEntries()

```
lock(mu_)

if request.term < current_term_:
    reject (stale leader)
    return {term, success=false}

if request.term > current_term_ OR state_ != FOLLOWER:
    BecomeFollowerLocked(request.term, leader_id, leader_service_address)
else:
    update leader info
    ResetElectionDeadlineLocked()

// Consistency check
prev_index = request.prev_log_index
if prev_index > 0:
    if prev_index >= log_.size():
        // follower log is shorter than expected
        reject with conflict_index = log_.size()
        return
    if log_[prev_index].term != request.prev_log_term:
        // term mismatch at prev_index
        find first index of the conflicting term (fast rollback hint)
        reject with {conflict_term, conflict_index}
        return

// Apply entries
FOR EACH entry in request.entries:
    index = prev_index + 1 + i
    if index < log_.size():
        if log_[index].term != entry.term:
            log_.resize(index)     // truncate conflicting suffix
        else:
            continue               // already have this entry
    log_.push_back(entry)

// Advance commit index
if request.leader_commit > commit_index_:
    commit_index_ = min(request.leader_commit, log_.size() - 1)
    notify commit_cv_ and apply_cv_

return {term, success=true, match_index = log_.size() - 1}
```

---

## 7. Commit and Apply

### 7.1 AdvanceCommitIndexLocked()

Called by the leader after a successful `ReplicateToFollower`.

```
if not LEADER: return

FOR index from (log_.size() - 1) down to (commit_index_ + 1):
    if log_[index].term != current_term_:
        continue   // Raft safety: only commit entries from the current term
    replicated = 1 (self)
    FOR EACH peer:
        if match_index_[peer] >= index: ++replicated
    if replicated >= QuorumSize():
        commit_index_ = index
        notify commit_cv_ and apply_cv_
        break
```

**Important:** The Raft safety rule means a leader **never** commits entries from
a previous term directly.  They can only be committed indirectly when a new
entry from the current term reaches quorum and "carries" older entries forward
(because `commit_index_` jumps to the new entry's index, which is past the old
entries).

### 7.2 Propose(entry, timeout)

The leader's primary interface for adding entries to the log.

```
lock(mu_)
    if not LEADER or not running_: return false
    stamp entry with current_term_
    log_.push_back(entry)
    proposed_index = log_.size() - 1
    if single-node: commit immediately, return true
unlock(mu_)

deadline = now + timeout
WHILE running_ and now < deadline:
    spawn one thread per peer → ReplicateToFollower(peer)
    join all threads

    lock(mu_)
    if commit_index_ >= proposed_index:
        set *committed_index = proposed_index
        return true
    if not LEADER: return false

    wait on commit_cv_ for up to 25ms
    unlock(mu_)

// Final check after timeout
lock(mu_)
if commit_index_ >= proposed_index:
    return true
return false
```

**Critical behavior:** The entry is appended to `log_` **before** any
replication attempt.  If `Propose` times out and returns `false`, the entry
**remains in the log** and will continue to be replicated by the heartbeat loop.
This is why the compensating RELEASE mechanism exists (see §11).

### 7.3 AppendLocalEntry(entry)

Fire-and-forget variant of Propose.

```
lock(mu_)
if not LEADER or not running_: return false
stamp entry with current_term_
log_.push_back(entry)
if single-node: commit immediately
return true
```

No replication loop.  The entry will be replicated by the next heartbeat cycle.
Used exclusively for compensating RELEASE entries (§11).

### 7.4 WaitUntilApplied(index, timeout)

```
lock(mu_)
wait on apply_cv_ until (last_applied_ >= index) or !running_ or timeout
return (last_applied_ >= index)
```

---

## 8. The on_commit_ Callback and Lock Table Integration

Defined in `main.cpp` as a lambda:

```cpp
[&lock_table](const dscc_raft::LogEntry& entry) {
    if (entry.op_type() == ACQUIRE) {
        lock_table.apply_acquire(entry.agent_id(), embedding, entry.theta());
    } else {
        lock_table.apply_release(entry.agent_id());
    }
}
```

This runs on the `ApplyLoop` thread on **every** node (leader and followers).

### 8.1 apply_acquire

- Acquires `mu_` on the lock table.
- If the agent_id already exists in `active_`: updates centroid, threshold,
  **sets `pending = false`** (promotion from pending → real).
- If the agent_id does not exist: inserts a new `SemanticLock` with
  `pending = false`.

On the **leader**, the agent_id will typically already exist as a pending slot
(inserted by `wait_for_admission`).  `apply_acquire` promotes it.

On **followers**, the agent_id will not exist.  `apply_acquire` inserts it fresh.

### 8.2 apply_release

- Delegates to `release(agent_id)`.
- `release` acquires `mu_`, calls `remove_lock_locked` to remove the entry and
  harvest its waiter queue, then calls `rebalance_waiters_locked`.
- If the agent_id is not found: logs `"WARN: release called for unknown
  agent_id <id>"` and returns.  This is a no-op, not an error.

---

## 9. The AcquireGuard Request Lifecycle (Three Phases)

This is the most important code path.  Located in `lock_service_impl.cpp`.

### 9.1 Pre-checks

```
1. Parse request fields (agent_id, embedding, payload_text, operation_type, ...)
2. Validate: agent_id and embedding must be non-empty
3. Leadership check: if raft_ != nullptr and not leader:
       return FAILED_PRECONDITION "NOT_LEADER" + leader-address metadata
4. LOG: "[TX <agent_id>] attempting acquire op=<write|read>"
```

### 9.2 Phase 1 — Admission Gate

```
AcquireTrace = lock_table_->wait_for_admission(agent_id, embedding, theta_)
```

This blocks the calling thread (gRPC thread pool) until no semantic conflict
exists, then reserves a **pending slot** in the lock table.

The pending slot:
- Has a centroid and threshold, so `find_conflict_locked` treats it as a real
  conflict for any subsequent incoming request.
- Has `pending = true`, which means it is not yet Raft-committed.
- Can be removed via `remove_pending` if the Raft Propose fails.

```
LOG: "[TX <agent_id>] admitted (pending, active count = <N>)"
```

Internally, if the request had to wait:
```
LOG: "[LOCK_QUEUE] agent=<id> waiting_on=<blocker> similarity=<sim> queue_position=<pos> theta=<θ>"
```

If granted via rebalance handoff during a release:
```
LOG: "[LOCK_GRANT] agent=<id> queue_hops=<hops> active_locks=<N>"
```

### 9.3 Phase 2 — Raft Commit + Apply

**If Raft is enabled (raft_ != nullptr):**

```
Step 2a: Propose(ACQUIRE entry, timeout=raft_propose_timeout_ms_)
    → If Propose FAILS:
        lock_table_->remove_pending(agent_id)
        AppendLocalEntry(compensating RELEASE)        // see §11
        return UNAVAILABLE "Raft quorum not reached for ACQUIRE"

Step 2b: WaitUntilApplied(acquire_log_index, timeout=raft_propose_timeout_ms_)
    → If WaitUntilApplied FAILS:
        AppendLocalEntry(compensating RELEASE)        // see §11
        return UNAVAILABLE "Raft apply timed out for ACQUIRE"
```

After successful WaitUntilApplied, the `on_commit_` callback has executed
`apply_acquire`, which promoted the pending slot to a real lock on the leader.
Followers also have the lock via their own `ApplyLoop`.

**If Raft is disabled (single-node, raft_ == nullptr):**

```
lock_table_->apply_acquire(agent_id, embedding, theta_)
```

Directly promotes pending → real.

```
LOG: "[TX <agent_id>] lock promoted (active count = <N>)"
```

### 9.4 Phase 3 — Qdrant + Release

From this point forward the lock is fully committed.  A `ScopeExit` guard
ensures cleanup on any exit path.

```
Step 3a: Qdrant operation
    - WRITE: upsert_embedding_to_qdrant (FNV-1a hash ID, 3 retries, 75ms backoff)
    - READ:  query_embedding_from_qdrant

Step 3b: Lock hold sleep (only for WRITE, if LOCK_HOLD_MS > 0)
    std::this_thread::sleep_for(lock_hold_ms_)

Step 3c: commit_release_once()
    Propose(RELEASE) + WaitUntilApplied
    LOG: "[TX <agent_id>] released lock (active count = <N>)"

Step 3d: Return AcquireResponse with granted=true
```

**ScopeExit guard:** If the function returns early (Qdrant failure, etc.) after
Phase 2 succeeded but before `commit_release_once` was called, the destructor
calls `commit_release_once` to prevent lock leaks.

```
If ScopeExit cleanup fails:
    LOG: "[TX <agent_id>] release replication failed during cleanup"
```

### 9.5 Complete Raft Log Sequence for One Successful Request

```
log_[i]   = {term=T, op=ACQUIRE, agent_id="A", embedding=[...], theta=0.85}
log_[i+1] = {term=T, op=RELEASE, agent_id="A"}
```

Both entries are committed at quorum and applied on all nodes in order.

---

## 10. The ReleaseGuard Request Lifecycle

A standalone release RPC (separate from the release inside AcquireGuard).

```
1. Validate agent_id
2. Leadership check (same as AcquireGuard)
3. If Raft enabled:
     Propose(RELEASE) + WaitUntilApplied
   Else:
     lock_table_->release(agent_id)
4. LOG: "[TX <agent_id>] released lock (active count = <N>)"
5. Return ReleaseResponse with success=true
```

---

## 11. Compensating RELEASE — Ghost Lock Prevention

### 11.1 The Problem

`Propose` appends the entry to `log_` **before** attempting replication.  If
`Propose` times out, the entry remains in the log and will eventually be
replicated by heartbeats.  If it reaches quorum after the caller already
returned an error, `ApplyLoop` calls `apply_acquire` — creating a "ghost lock"
that no client session owns and no RELEASE will ever clean up.

### 11.2 The Solution

When `Propose(ACQUIRE)` fails, the service immediately:

1. Calls `lock_table_->remove_pending(agent_id)` to clean up the local pending
   slot.
2. Calls `raft_->AppendLocalEntry(RELEASE for agent_id)` to inject a
   compensating RELEASE into the log.

Similarly, when `WaitUntilApplied` times out for ACQUIRE (meaning the ACQUIRE is
committed but not yet applied), a compensating RELEASE is appended.

### 11.3 Why This Is Safe

The compensating RELEASE has a **higher log index** than the ACQUIRE, so Raft
guarantees it will be applied **after** the ACQUIRE on every node.  Three
possible timelines:

| Timeline | What Happens | Outcome |
|----------|-------------|---------|
| ACQUIRE never committed | Both entries sit uncommitted; if leader changes, a new leader may truncate them. If they do commit eventually, ACQUIRE is applied then RELEASE undoes it. | Safe |
| ACQUIRE commits, then RELEASE commits | `apply_acquire` runs (inserts lock), then `apply_release` runs (removes it). Brief transient lock between the two applies. | Safe |
| RELEASE applied for unknown agent | `release()` → `remove_lock_locked` finds no entry → logs a warning, returns. | Safe (harmless no-op) |

### 11.4 AppendLocalEntry Details

`AppendLocalEntry` does NOT wait for quorum.  It only appends to the leader's
local log and returns.  The `HeartbeatLoop` (which calls `ReplicateToFollower`
every 75ms) will replicate it to followers on its next cycle.  This is
acceptable because:

- The entry is guaranteed to be in the leader's log.
- If the leader remains leader, it will be replicated and committed.
- If the leader loses leadership, the new leader may or may not have this entry.
  If the new leader does NOT have it and the ghost ACQUIRE was committed, the
  ghost lock persists until TTL/lease (not yet implemented — see §16).

---

## 12. Pending Lock Semantics

### 12.1 What is a Pending Lock?

A `SemanticLock` with `pending = true`.  It exists in the `active_` map and
participates in conflict detection (cosine similarity checks) but represents a
**reservation**, not a Raft-committed lock.

### 12.2 Lifecycle

```
1. wait_for_admission → insert_pending_locked → pending=true in active_
2. Propose(ACQUIRE) + WaitUntilApplied → on_commit_ → apply_acquire → pending=false
   OR
2'. Propose fails → remove_pending → removed from active_
```

### 12.3 Why Pending Locks Exist

Between the moment `wait_for_admission` returns "no conflict" and the moment
`apply_acquire` runs, another request could see no conflict and slip through.
The pending slot **blocks** those requests at `find_conflict_locked` because it
has a valid centroid and threshold.

### 12.4 Rebalance Behavior

When a lock is released and its wait queue is rebalanced:
- Waiters that have no remaining conflict get a **pending** slot
  (`insert_pending_locked`), not a real lock.
- The waiter's `wait_for_admission` call returns (with `granted = true`).
- The calling thread then proceeds to Propose(ACQUIRE) through Raft, which
  promotes the pending slot to real.

This ensures that even waiters granted via rebalance handoff go through the
full Raft commit path.

### 12.5 remove_pending

```
lock(mu_)
find agent_id in active_
if not found OR not pending: return (no-op)
harvest waiters from the entry
erase entry
rebalance_waiters_locked(waiters)  → grants new pending slots to eligible waiters
unlock(mu_)
notify granted waiters
```

If the lock was already promoted (pending=false), `remove_pending` is a no-op.
This handles the race where `apply_acquire` (from the ApplyLoop thread) promotes
the lock between the `Propose` failure and the `remove_pending` call.  In that
case, the compensating RELEASE (via `AppendLocalEntry`) handles cleanup through
the Raft path instead.

---

## 13. Failure Scenarios — Exhaustive Catalog

### Scenario 1: Happy Path (all succeeds)

```
Thread  │ Lock Table        │ Raft Log (leader)  │ Raft Log (followers)
────────┼───────────────────┼────────────────────┼─────────────────────
1       │ wait_for_admission│                    │
        │   → pending=true  │                    │
2       │                   │ ACQUIRE appended   │
        │                   │ replicated→quorum  │ ACQUIRE replicated
3       │ apply_acquire     │                    │ apply_acquire
        │   → pending=false │                    │   → new lock
4       │ (Qdrant op)       │                    │
5       │                   │ RELEASE appended   │
        │                   │ replicated→quorum  │ RELEASE replicated
6       │ apply_release     │                    │ apply_release
        │   → lock removed  │                    │   → lock removed
```

Logs:
```
[TX A] attempting acquire op=write
[LOCK_QUEUE] agent=A waiting_on=... (only if conflict existed)
[TX A] admitted (pending, active count = 1)
[TX A] lock promoted (active count = 1)
[TX A] released lock (active count = 0)
```

### Scenario 2: Propose(ACQUIRE) Times Out

```
Thread  │ Lock Table (leader) │ Raft Log         │ Effect
────────┼─────────────────────┼──────────────────┼────────────────────
1       │ pending=true        │                  │
2       │                     │ ACQUIRE appended │ Propose timeout (5s)
3       │ remove_pending      │                  │ pending slot removed
4       │                     │ RELEASE appended │ compensating entry
5       │ return UNAVAILABLE  │                  │
────────┼─────────────────────┼──────────────────┼────────────────────
Later   │                     │ ACQUIRE committed│ (heartbeats replicated it)
        │ apply_acquire       │                  │ lock inserted
        │                     │ RELEASE committed│
        │ apply_release       │                  │ lock removed (cleanup)
```

The ghost ACQUIRE is cancelled by the compensating RELEASE.

### Scenario 3: WaitUntilApplied Times Out for ACQUIRE

```
Thread  │ Lock Table (leader) │ Raft Log          │ Effect
────────┼─────────────────────┼───────────────────┼────────────────────
1       │ pending=true        │                   │
2       │                     │ ACQUIRE committed │ Propose succeeded
3       │                     │                   │ WaitUntilApplied timeout
4       │                     │ RELEASE appended  │ compensating entry
5       │ return UNAVAILABLE  │                   │
────────┼─────────────────────┼───────────────────┼────────────────────
Later   │ apply_acquire       │                   │ promotes pending→real
        │ apply_release       │                   │ lock removed
```

ACQUIRE was committed and will be applied.  The compensating RELEASE ensures
cleanup.  There is a brief window where the lock is real (between apply_acquire
and apply_release), but no client holds it.

### Scenario 4: Propose(RELEASE) Fails at End of Request

```
Thread  │ Lock Table (leader) │ Raft Log          │ Effect
────────┼─────────────────────┼───────────────────┼────────────────────
1-4     │ (normal acquire)    │ ACQUIRE committed │
        │                     │ ACQUIRE applied   │
5       │ Qdrant op succeeds  │                   │
6       │ commit_release_once │ RELEASE appended  │ Propose timeout
        │                     │                   │ → return false
7       │ ScopeExit fires     │                   │ tries commit_release_once again
        │                     │                   │ → also fails (already tried)
8       │                     │                   │ LOG: "release replication
        │                     │                   │   failed during cleanup"
9       │ return UNAVAILABLE  │                   │
```

**PROBLEM:** The Qdrant write succeeded but the lock is stuck.  The RELEASE
entry is in the log and may eventually commit (cleaning up the lock), but
there is no guarantee.  If the leader loses leadership, the stuck lock persists
on the new leader.  **This requires TTL/lease expiration to fix (not yet
implemented).**

### Scenario 5: Leader Crashes Between Phase 1 and Phase 2

```
Leader  │ Lock Table          │ Raft Log          │
────────┼─────────────────────┼───────────────────┼
1       │ pending=true        │                   │
        │       ** CRASH **   │                   │
────────┼─────────────────────┼───────────────────┼
New     │ (empty table)       │ (no ACQUIRE entry)│
Leader  │                     │                   │
```

The pending slot was in-memory only, never replicated.  Clean state on the new
leader.  The client's gRPC times out; the proxy retries on the new leader.
**Safe.**

### Scenario 6: Leader Crashes After ACQUIRE Committed, Before Qdrant

```
Leader  │ Lock Table          │ Raft Log          │
────────┼─────────────────────┼───────────────────┼
1       │ pending=true        │                   │
2       │                     │ ACQUIRE committed │
3       │ pending→real        │                   │ (apply ran)
        │       ** CRASH **   │                   │
────────┼─────────────────────┼───────────────────┼
New     │ lock exists (from   │ ACQUIRE in log    │
Leader  │   replayed apply)   │                   │
```

**PROBLEM:** The lock is held on the new leader (via `apply_acquire` during log
replay) but no agent is connected to release it.  The client's RPC timed out.
If the client retries the same agent_id, `wait_for_admission` will conflict with
the agent's own lock.  **Deadlock** unless:
- The RELEASE was also committed (unlikely — we crashed before Qdrant).
- TTL/lease expiration cleans it up (not yet implemented).

### Scenario 7: Leader Crashes After Qdrant, Before RELEASE Committed

Same as Scenario 6 but worse: the Qdrant write **did** happen.

```
New     │ lock exists          │ ACQUIRE in log    │
Leader  │ (no RELEASE in log)  │                   │
        │                      │                   │
Qdrant  │ has the point        │                   │
```

The data is in Qdrant but the lock is stuck.  **Requires TTL/lease.**

### Scenario 8: Follower Falls Behind, Then Catches Up

```
Follower is down.  Leader commits entries [i..j].
Follower restarts.

HeartbeatLoop:
    ReplicateToFollower(follower):
        next_index_[follower] = log_.size()    // from BecomeLeaderLocked
        send AppendEntries with prev_log_index = next_index - 1

Follower HandleAppendEntries:
    prev_log_index check may fail (follower doesn't have that index)
    → reject with conflict_index = log_.size()   (follower's actual length)

Leader ReplicateToFollower:
    next_index_[follower] = max(1, conflict_index)
    retry with earlier entries

Eventually: follower has all entries, commit_index advances, ApplyLoop catches up.
```

### Scenario 9: Network Partition — Leader in Minority

```
Cluster: [node-1, node-2, node-3, node-4, node-5]
Partition: {node-1(leader)} | {node-2, node-3, node-4, node-5}

node-1 (old leader):
    Propose → cannot reach quorum (only 1 of 3 needed)
    → Propose times out, returns false
    → Clients get UNAVAILABLE errors

{node-2..5}:
    election_deadline_ expires
    StartElection in new term
    one of them becomes leader (can reach quorum of 3)
    start serving requests

When partition heals:
    node-1 receives AppendEntries or VoteRequest with higher term
    → BecomeFollowerLocked
    → log may be truncated if it has uncommitted entries from old term
```

### Scenario 10: Split Vote (Election Fails)

```
node-1 and node-2 both start elections in the same term.
node-3, node-4, node-5 each vote for whoever they receive first.

If no candidate gets quorum:
    All candidates' election_deadline_ expires
    New election with higher term
    Randomized timeouts (600-1000ms) make split votes unlikely to repeat
```

Log output during livelock looks like:
```
[RAFT node-1] starting election term=5
[RAFT node-2] starting election term=5
[RAFT node-3] starting election term=5
[RAFT node-1] starting election term=6
...
```

### Scenario 11: Two Agents, One Semantic Conflict

```
Agent A and Agent B have cosine_similarity(A, B) >= θ.
Both send AcquireGuard simultaneously to the leader.

Thread A:
    wait_for_admission("A") → no conflict → pending slot for A
    Propose(ACQUIRE A) → committed → apply → promoted

Thread B:
    wait_for_admission("B") → conflict with A (pending or real)
    → waiter B added to A's queue
    → blocks on condition variable

(Thread A continues: Qdrant, RELEASE)
    apply_release("A") → release:
        remove A from active_
        rebalance_waiters_locked:
            B has no conflict now → insert_pending_locked("B")
            B.granted = true, B.ready = true
        notify B's CV

Thread B wakes:
    wait_for_admission returns (granted by handoff, pending slot exists)
    Propose(ACQUIRE B) → committed → apply → promoted
    Qdrant, RELEASE
```

Logs:
```
[TX A] attempting acquire op=write
[TX A] admitted (pending, active count = 1)
[TX B] attempting acquire op=write
[LOCK_QUEUE] agent=B waiting_on=A similarity=0.892 queue_position=1 theta=0.850
[TX A] lock promoted (active count = 1)
[TX A] released lock (active count = 0)
[LOCK_GRANT] agent=B queue_hops=0 active_locks=1
[TX B] admitted (pending, active count = 1)
[TX B] lock promoted (active count = 1)
[TX B] released lock (active count = 0)
```

### Scenario 12: remove_pending Race with apply_acquire

```
Thread (gRPC):                  ApplyLoop thread:
    Propose(ACQUIRE) fails          (entry is in log, may be replicating)
    |                               |
    remove_pending(agent_id)        apply_acquire(agent_id)
    |                               |
    ???                             ???
```

**Case A:** `remove_pending` runs first.
- Finds pending=true, removes it, rebalances waiters.
- `apply_acquire` runs later: agent_id not found → inserts new real lock.
- Compensating RELEASE (from `AppendLocalEntry`) eventually applies → removes it.

**Case B:** `apply_acquire` runs first.
- Promotes pending→real (sets pending=false).
- `remove_pending` runs: finds pending=false → **no-op** (returns immediately).
- Compensating RELEASE eventually applies → removes the real lock.

Both cases converge to the correct state: lock is cleaned up.

### Scenario 13: Compensating RELEASE but Leader Loses Leadership

```
1. Leader appends ACQUIRE at index i
2. Propose times out → leader appends compensating RELEASE at index i+1
3. Leader loses leadership before RELEASE reaches quorum
4. New leader may or may not have index i+1

If new leader HAS i+1: both commit and apply in order → safe
If new leader does NOT have i+1:
    ACQUIRE at index i may commit (if it reached quorum)
    → ghost lock with no compensating RELEASE
    → requires TTL/lease to clean up
```

This is a known gap.  The compensating RELEASE is best-effort.

---

## 14. Log Messages Reference

### 14.1 Raft Layer (raft_node.cpp)

| Message | When |
|---------|------|
| `[RAFT <node>] starting election term=<T>` | ElectionTimerLoop triggers StartElection |
| `[RAFT <node>] became leader term=<T>` | Node wins election with quorum votes |

### 14.2 Lock Table Layer (active_lock_table.cpp)

| Message | When |
|---------|------|
| `[LOCK_QUEUE] agent=<id> waiting_on=<blocker> similarity=<sim> queue_position=<pos> theta=<θ>` | Request blocked by a semantic conflict |
| `[LOCK_REQUEUE] agent=<id> waiting_on=<new_blocker> similarity=<sim> queue_position=<pos> queue_hops=<hops> theta=<θ>` | During rebalance, waiter moved to a different lock's queue |
| `[LOCK_GRANT] agent=<id> queue_hops=<hops> active_locks=<N>` | During rebalance, waiter granted (no remaining conflict) |
| `ActiveLocks: [<id1>, <id2>, ...]` | After any acquire, release, or rebalance event |
| `WARN: release called for unknown agent_id <id>` | Release for an agent not in the table (harmless) |

### 14.3 Lock Service Layer (lock_service_impl.cpp)

| Message | When |
|---------|------|
| `[TX <id>] attempting acquire op=<write\|read>` | AcquireGuard RPC received |
| `[TX <id>] admitted (pending, active count = <N>)` | wait_for_admission returned |
| `[TX <id>] lock promoted (active count = <N>)` | Raft ACQUIRE committed + applied |
| `[TX <id>] released lock (active count = <N>)` | Raft RELEASE committed + applied |
| `[TX <id>] release replication failed during cleanup` | ScopeExit failed to commit RELEASE |

### 14.4 Qdrant Layer (lock_service_impl.cpp)

| Message | When |
|---------|------|
| `[QDRANT] request failed for agent_id=<id> target=<url> attempt=<N>` | HTTP request to Qdrant failed |
| `[QDRANT] upsert failed for agent_id=<id> status=<code> attempt=<N> response=<body>` | Qdrant returned non-2xx for upsert |
| `[QDRANT] read request failed for agent_id=<id> target=<url>` | HTTP request for search failed |
| `[QDRANT] DNS resolution failed for host=<host> error=<msg>` | Cannot resolve Qdrant hostname |
| `[QDRANT] connect failed host=<host> port=<port> errno=<N> message=<msg>` | TCP connection to Qdrant failed |

---

## 15. Configuration Reference

All values are read from environment variables in `main.cpp`.

| Variable | Default | Description |
|----------|---------|-------------|
| `NODE_ID` | `"node-1"` | Unique Raft node identifier |
| `PORT` | `"50051"` | Client-facing gRPC port |
| `RAFT_PORT` | `"50061"` | Inter-node Raft gRPC port |
| `ADVERTISE_HOST` | `"127.0.0.1"` | Hostname for leader redirect metadata |
| `PEERS` | `""` | Comma-separated Raft peer addresses (e.g. `node-2:50062,node-3:50063`) |
| `THETA` | `0.85` | Cosine similarity threshold (range 0.0–1.0) |
| `LOCK_HOLD_MS` | `0` | Simulated critical section hold time after Qdrant write |
| `RAFT_PROPOSE_TIMEOUT_MS` | `5000` | Max time for Propose to wait for quorum commit |
| `HEARTBEAT_MS` | `75` | Leader heartbeat interval |
| `ELECTION_TIMEOUT_MIN_MS` | `600` | Randomized election timeout lower bound |
| `ELECTION_TIMEOUT_MAX_MS` | `1000` | Randomized election timeout upper bound |
| `RAFT_RPC_TIMEOUT_MS` | `150` | Per-RPC deadline for AppendEntries / RequestVote |
| `QDRANT_HOST` | `"qdrant"` | Qdrant server hostname |
| `QDRANT_PORT` | `"6333"` | Qdrant server port |
| `QDRANT_COLLECTION` | `"dscc_memory"` | Qdrant collection name |

### Timing Relationships

```
HEARTBEAT_MS (75ms)  <<  ELECTION_TIMEOUT_MIN (600ms)
                          ratio = 8×  (Raft recommends 5-10×)

RAFT_RPC_TIMEOUT (150ms)  ~  2× HEARTBEAT_MS
RAFT_PROPOSE_TIMEOUT (5000ms)  >>  HEARTBEAT_MS
```

---

## 16. Known Limitations and Open Issues

### 16.1 No Durable State (Critical)

All Raft state (`current_term_`, `voted_for_`, `log_`) is in-memory.  If a node
restarts, it starts fresh with term=0, empty log, no vote record.  If all nodes
restart simultaneously, all lock state is lost.

**Violations:**
- Raft requires `current_term_` and `voted_for_` to survive restarts.  Without
  durability, a restarted node could vote twice in the same term, allowing two
  leaders.
- Without a persisted log, a restarted node cannot contribute its entries to
  quorum decisions.

**Fix:** Write-ahead log + metadata file with fsync (Fault 4 from prior analysis).

### 16.2 InstallSnapshot Is a No-Op (Critical)

`HandleInstallSnapshot` only updates the term.  It does not install any state.
This means:
- Log compaction is impossible (log grows without bound).
- A follower that falls too far behind (beyond any future compaction point)
  cannot catch up.

**Fix:** Periodic lock-table snapshots + real snapshot transfer (Fault 5).

### 16.3 No Lease/TTL for Stuck Locks (High)

If a lock is committed via Raft but the corresponding RELEASE never commits
(leader crash, network issue, etc.), the lock persists indefinitely.  There is
no heartbeat or timeout mechanism to auto-release abandoned locks.

This affects Scenarios 4, 6, 7, and 13 above.

**Fix:** Lock leases with expiration timestamps, checked periodically.

### 16.4 Compensating RELEASE Is Best-Effort (Medium)

`AppendLocalEntry` does not wait for quorum.  If the leader loses leadership
before the compensating RELEASE is replicated, the new leader may not have it.
A ghost ACQUIRE that was committed survives as a stuck lock (see Scenario 13).

**Mitigation:** Combined with TTL/lease, this becomes tolerable.  The
compensating RELEASE handles the common case (leader remains leader); TTL
handles the uncommon case (leadership changes during cleanup).

### 16.5 Sequential Vote Collection (Low)

`StartElection` collects votes sequentially (one peer at a time).  With 4 peers
and 150ms RPC timeout per peer, a worst-case election round takes 600ms.  This
is within the election timeout range (600-1000ms) but leaves little margin.

**Potential fix:** Parallelize vote requests.

### 16.6 Thread-per-Peer-per-Heartbeat (Low)

Both `HeartbeatLoop` and `Propose` spawn N threads (one per peer) per replication
cycle and join them all before proceeding.  This is simple but creates O(heartbeat_rate × peer_count) short-lived threads.

**Potential fix:** Thread pool or async gRPC.

### 16.7 No Log Entry Batching (Low)

Each `Propose` call appends exactly one entry and runs a full replication cycle.
Multiple concurrent `Propose` calls each trigger independent replication rounds
rather than batching entries.

**Potential fix:** Batch pending proposals and replicate in bulk.

### 16.8 RAFT_PROPOSE_TIMEOUT_MS Is Suspiciously High (Investigate)

5000ms for quorum on a local Docker network is 100-1000× higher than expected.
Possible causes:
- gRPC channel establishment overhead on first AppendEntries
- `on_commit_` callback contending with lock table mutex
- Thread scheduling delays in Docker Desktop
- No entry batching means serialized replication

The system works at 5000ms but this masks underlying performance issues.

---

## 17. In-Process Raft Regression Testbench (`dscc-raft-test`)

The binary `dscc-raft-test` (from `src/raft_test.cpp`) spins up **three real gRPC
servers** on `127.0.0.1`, each hosting a `RaftNode` + `RaftServiceImpl`.  It does
not link the lock table or Qdrant; it only exercises **election, quorum
replication, commit/apply ordering, leader failover, follower catch-up after a
cold restart, and `AppendLocalEntry` replication via heartbeats** — the same
paths documented in §5–§7 and failure catalog §13 (especially Scenario 8).

### 17.1 Why the old test was flaky

Raft’s `StartElection` collects votes **sequentially** with one RPC timeout per
peer (see §5.1).  For two peers, the worst-case vote phase is about
`2 × RAFT_RPC_TIMEOUT_MS`.  If `ELECTION_TIMEOUT_MAX_MS` is **smaller** than that,
the election timer can fire again while votes are still being collected, causing
extra elections and unstable leadership.  The testbench therefore uses
`election_timeout_min_ms = 500`, `election_timeout_max_ms = 800`, and
`rpc_timeout_ms = 200` so a full vote round fits comfortably inside one
election window.

### 17.2 Build and run

From a configured build directory (CMake already enables `dscc_proto` / gRPC for
this target):

```bash
cmake --build <build-dir> --target dscc-raft-test
./<build-dir>/dscc-raft-test
```

Docker / CI: ensure the container or job may **bind localhost TCP ports** in
the range used by the test (see §17.4).  If `BuildAndStart` fails, the suite logs
which addresses failed and marks the scenario as FAIL instead of aborting.

**Exit code:** `0` if every check in every scenario passed, `1` if any check
failed.  A final line prints `[RAFT-TEST] exit 0` or `exit 1`.

### 17.3 How to read the log

Each scenario starts with a banner:

`[RAFT-TEST] === <title> ===`

Individual assertions look like:

`[RAFT-TEST] S3: restarted follower applied all 12 entries: PASS`

or `: FAIL`.  Raft layer messages such as `[RAFT node-1] became leader term=2`
come from `log_line` in `raft_node.cpp` (§14.1).  At the end:

`[RAFT-TEST] SUMMARY: ALL PASS` or `SUMMARY: FAIL`.

When debugging a regression, grep for `: FAIL` first, then rerun with a single
scenario temporarily isolated in code if you need a shorter loop.

### 17.4 Scenario matrix (what each block proves)

| ID | Focus | Maps to RAFT_EXPL |
|----|--------|-------------------|
| **S1** | Single leader, stable heartbeats, one `Propose` + `WaitUntilApplied`, identical applied streams on all nodes | §7 (commit/apply), Scenario 1 |
| **S2** | Stop the current leader; replacement leader `Propose`s successfully; surviving replicas apply | §5 election, §13 Scenario 1 / leader loss |
| **S3** | Non-leader stopped; batch commit with quorum; **cold** replacement node on same addresses; follower log repair + apply; byte-identical applied logs vs leader | §6 replication, §13 **Scenario 8** |
| **S4** | `AppendLocalEntry` then eventual quorum commit via heartbeats; all nodes apply same sequence | §6.1 / §7.3, §11.4 |
| **S5** | Lock-shaped **ACQUIRE** then **RELEASE** on one `agent_id`; all nodes match | §9.5 log shape |
| **S6** | Many sequential `Propose` calls with all peers up; stress on replication + apply | §6–§7 throughput path |
| **S7** | After leader is known, short window: never more than one `IsLeader()` among live nodes | Sanity check (not a formal safety proof) |

Port layout (per process, derived from `getpid()` so parallel invocations rarely
collide): a block base `B` yields Raft ports `B+10`, `B+11`, `B+12` and client
ports `B`, `B+1`, `B+2`.  The first log line prints the concrete addresses.

### 17.5 Limitations of this testbench

- It does **not** persist state: restarted nodes match production “blank disk”
  behavior (§16.1) and validate catch-up from a live leader, not crash recovery.
- **InstallSnapshot** is not meaningfully exercised (still a stub in production).
- No network partition with **two** simultaneous leaders is attempted; S7 only
  checks the common case on a healthy LAN.

When S3 or S4 fails, inspect **log replication** (`ReplicateToFollower`,
`HandleAppendEntries`) and **commit rules** (`AdvanceCommitIndexLocked`).  When
S2 fails, inspect **election timing** and vote handling.  When S5/S6 fail,
inspect **apply ordering** and `Propose`’s interaction with the heartbeat loop.
