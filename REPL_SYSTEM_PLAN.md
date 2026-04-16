# DSCC — Replicated Semantic Lock Manager: Complete Architecture Plan

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Per-Lock Wait Queue Redesign](#2-per-lock-wait-queue-redesign)
3. [Raft Consensus Integration](#3-raft-consensus-integration)
4. [Replication Message Protocol](#4-replication-message-protocol)
5. [Leader-Aware Proxy](#5-leader-aware-proxy)
6. [New Component: RaftNode](#6-new-component-raftnode)
7. [Updated LockServiceImpl](#7-updated-lockserviceimpl)
8. [Updated Proto Contract](#8-updated-proto-contract)
9. [Data Flow: Full Request Lifecycle](#9-data-flow-full-request-lifecycle)
10. [Leader Election: Step-by-Step](#10-leader-election-step-by-step)
11. [Failure Scenarios and Recovery](#11-failure-scenarios-and-recovery)
12. [Updated Repository Structure](#12-updated-repository-structure)
13. [Updated Docker Compose Stack](#13-updated-docker-compose-stack)
14. [Build System Changes](#14-build-system-changes)
15. [Configuration Reference](#15-configuration-reference)
16. [Implementation Roadmap](#16-implementation-roadmap)
17. [Known Limitations and Future Work](#17-known-limitations-and-future-work)

---

## 1. System Overview

### 1.1 What Changes

The current system has one `dscc-node` process containing a single in-memory `ActiveLockTable`. This plan replaces it with a cluster of three `dscc-node` processes, each running identical application logic but participating in a Raft consensus group to maintain a consistent, replicated view of the active lock table.

A new lightweight proxy component (`dscc-proxy`) sits in front of the three nodes. Agents no longer address `dscc-node` directly — they address `dscc-proxy`, which forwards requests to whichever node is currently the Raft leader.

The starvation problem is fixed independently of replication, via a redesigned `ActiveLockTable` that replaces the single global condition variable with a per-lock wait queue. This change is self-contained and can be implemented and tested before any distributed work begins.

### 1.2 Cluster Topology

```
                         Agents
                            │
                            ▼
                     ┌─────────────┐
                     │ dscc-proxy  │  Port 50050
                     │ (Envoy or   │  Leader-aware routing
                     │  custom Go) │
                     └──┬──┬──┬───┘
                        │  │  │
           ┌────────────┘  │  └──────────────┐
           │               │                 │
           ▼               ▼                 ▼
    ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
    │  dscc-node  │ │  dscc-node  │ │  dscc-node  │
    │  (node-1)   │ │  (node-2)   │ │  (node-3)   │
    │  Port 50051 │ │  Port 50052 │ │  Port 50053 │
    │             │ │             │ │             │
    │  RaftNode   │ │  RaftNode   │ │  RaftNode   │
    │  LockSvc    │ │  LockSvc    │ │  LockSvc    │
    │  LockTable  │ │  LockTable  │ │  LockTable  │
    └──────┬──────┘ └──────┬──────┘ └──────┬──────┘
           │               │               │
           └───────────────┴───────────────┘
                    Raft RPC mesh
                  (peer-to-peer gRPC)
                        │
                        ▼
                  ┌───────────┐
                  │  Qdrant   │
                  │  Port6333 │
                  └───────────┘
```

### 1.3 Roles

**Leader node:** The only node that accepts `AcquireGuard` RPCs from the proxy. Runs the conflict-check, updates the local lock table, replicates the operation to followers via Raft log, and writes to Qdrant only after a quorum of followers have acknowledged the log entry.

**Follower nodes:** Receive replicated log entries from the leader. Apply them to their local lock tables. Do not accept `AcquireGuard` RPCs from agents (proxy ensures this). Available to be elected as leader if the current leader becomes unavailable.

**Proxy:** Knows which node is the current leader. Routes all `AcquireGuard` and `ReleaseGuard` RPCs to the leader. Routes `Ping` to any live node. Detects leader changes (via heartbeat or redirect responses) and updates routing accordingly.

---

## 2. Per-Lock Wait Queue Redesign

This section covers changes to `active_lock_table.h` and `active_lock_table.cpp` only. It has no dependency on Raft and should be implemented and validated first.

### 2.1 The Problem with the Current Design

The current `ActiveLockTable` has a single global `std::condition_variable cv_`. When any lock is released, `cv_.notify_all()` wakes every blocked thread in the entire system. Every thread then re-runs its full conflict scan. Only those whose specific blocking lock was released can make progress. All others immediately block again on `cv_.wait()`.

This produces two related problems. First, it is a thundering herd — N blocked agents all wake, scan, and re-sleep when only one or a few can proceed. Second, and more seriously, it causes starvation: agent C, which conflicts with nothing currently held, will nevertheless wake and re-sleep repeatedly as long as any other agents are blocked on the same condition variable, even for locks that have nothing to do with C's embedding.

Actually, on re-examination, the starvation problem in the current code is that agent C cannot acquire if A is waiting for B's lock to release, because C's `acquire` loop also sleeps on the same `cv_` as A. When B releases, both A and C wake. If A gets the mutex first, A acquires, then C wakes again and acquires — this works eventually. The real problem is throughput and fairness under high contention, not hard starvation. The per-lock queue fixes both the thundering herd and introduces FIFO fairness within a conflict group.

### 2.2 New Data Structures

**File: `src/active_lock_table.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <optional>

// One entry per agent currently waiting for a specific held lock to release.
struct WaitQueueEntry {
    std::string waiting_agent_id;
    std::vector<float> embedding;
    float theta;
    // The condition variable this waiter sleeps on.
    // Owned here so the lock holder can notify only this group.
    std::shared_ptr<std::condition_variable> cv;
    bool ready = false;  // set to true by notifier before cv->notify_all()
};

// One entry per currently held semantic lock.
struct SemanticLock {
    std::string agent_id;
    std::vector<float> centroid;
    float threshold;

    // FIFO queue of agents waiting specifically because they conflict with
    // this lock. When this lock is released, the front of this queue is
    // notified. The rest remain sleeping.
    std::deque<std::shared_ptr<WaitQueueEntry>> waiters;
};

struct AcquireTrace {
    bool waited = false;
    float blocking_similarity_score = 0.0f;
    std::string blocking_agent_id;
    int wait_position = 0;  // position in the wait queue when first blocked
};

class ActiveLockTable {
public:
    // Acquire a lock for agent_id with the given embedding and conflict
    // threshold. Blocks the calling thread if any active lock conflicts.
    // Returns an AcquireTrace with blocking metadata.
    AcquireTrace acquire(const std::string& agent_id,
                         const std::vector<float>& embedding,
                         float theta);

    // Release the lock held by agent_id. Wakes the next waiter in the
    // wait queue of this specific lock, if any.
    void release(const std::string& agent_id);

    // Apply a replicated acquire from Raft log (no blocking, no conflict
    // check — the leader already ran the check before committing to log).
    void apply_acquire(const std::string& agent_id,
                       const std::vector<float>& embedding,
                       float theta);

    // Apply a replicated release from Raft log.
    void apply_release(const std::string& agent_id);

    // Read-only snapshot for observability/debugging.
    std::vector<std::string> active_agent_ids() const;
    size_t active_count() const;

    static float cosine_similarity(const std::vector<float>& a,
                                   const std::vector<float>& b);

private:
    // Returns the SemanticLock that conflicts with the given embedding at
    // threshold theta, or nullptr if none. Returns the highest-similarity
    // conflict if multiple exist.
    SemanticLock* find_conflict(const std::vector<float>& embedding,
                                float theta);

    void print_active_locks() const;

    mutable std::mutex mu_;
    // Keyed by agent_id for O(1) lookup on release.
    std::unordered_map<std::string, SemanticLock> active_;
};
```

### 2.3 New Acquire Algorithm

**File: `src/active_lock_table.cpp`**

The acquire algorithm now uses a per-lock condition variable rather than a global one.

```
ACQUIRE(agent_id, embedding, theta):
  lock(mu_)

  LOOP:
    conflict = find_conflict(embedding, theta)

    IF conflict == nullptr:
      BREAK                   // no conflict, we can proceed

    // Build a wait entry and enqueue it on the conflicting lock's waiter list.
    entry = make_shared<WaitQueueEntry>{
        agent_id, embedding, theta,
        make_shared<condition_variable>(), false
    }
    conflict->waiters.push_back(entry)
    trace.waited = true
    trace.blocking_agent_id = conflict->agent_id
    trace.blocking_similarity_score = <computed similarity>
    trace.wait_position = conflict->waiters.size()

    log("[LOCK] " + agent_id + " queued behind " + conflict->agent_id)

    // Sleep until this specific entry is signaled.
    // The condition variable is owned by our entry; only the lock holder
    // that dequeues us will signal it.
    entry->cv->wait(lock, [&entry]{ return entry->ready; })

    // We were woken because our specific blocker released. But we must
    // re-run find_conflict because we might now conflict with a *different*
    // active lock that was granted while we were sleeping.
    GO TO LOOP

  // No conflict — insert into active table.
  active_[agent_id] = SemanticLock{agent_id, embedding, theta, {}}
  unlock(mu_)
  print_active_locks()
  RETURN trace
```

Key properties of this design:

- Each `WaitQueueEntry` has its own `condition_variable`. Only the agent directly ahead in the conflict queue will wake it.
- After being woken, the agent re-runs `find_conflict` to handle the case where a different lock was acquired by another agent during its sleep, which might now conflict with it.
- The re-check loop is bounded in practice because each iteration either grants the lock or re-queues the agent. In a well-behaved system with finite lock hold times, every agent eventually reaches the front of its queue and finds no conflict.
- The `apply_acquire` and `apply_release` methods are used by followers applying Raft log entries. They do not do conflict checks and do not wake waiters on followers (followers have no waiting agents — all agents connect to the leader).

### 2.4 New Release Algorithm

```
RELEASE(agent_id):
  lock(mu_)

  it = active_.find(agent_id)
  IF it == active_.end():
    log("WARN: release called for unknown agent_id " + agent_id)
    unlock(mu_)
    RETURN

  // Take the wait queue before erasing the lock entry.
  waiters = move(it->second.waiters)
  active_.erase(it)

  unlock(mu_)       // release the table lock before notifying

  print_active_locks()

  // Wake the first waiter in FIFO order.
  // That agent will re-run find_conflict. If it finds no conflict,
  // it will acquire and then propagate wakeup to its own first waiter.
  IF NOT waiters.empty():
    front = waiters.front()
    front->ready = true
    front->cv->notify_all()

    // Re-queue the remaining waiters onto whoever they now conflict with.
    // We do this under the table lock because we're modifying active_.
    lock(mu_)
    FOR entry IN waiters[1:]:
      conflict = find_conflict(entry->embedding, entry->theta)
      IF conflict != nullptr:
        conflict->waiters.push_front(entry)  // maintain FIFO: put at front
      ELSE:
        // No longer blocked — wake this agent too.
        entry->ready = true
        entry->cv->notify_all()
    unlock(mu_)
```

**Important note on waiter re-queuing:** When a lock releases and has N>1 waiters, only the first waiter is guaranteed to be unblocked by this release. The remaining waiters were queued because of *this* lock, but they may or may not conflict with the newly acquired lock that the first waiter is about to take. The release implementation above re-evaluates each remaining waiter's conflicts and either re-queues them (onto their new blocker) or wakes them immediately if they are now conflict-free.

This eliminates the thundering herd entirely: only the agents that can actually make progress are woken.

### 2.5 find_conflict

```cpp
SemanticLock* ActiveLockTable::find_conflict(
    const std::vector<float>& embedding, float theta)
{
    // mu_ must be held by caller.
    SemanticLock* best = nullptr;
    float best_sim = -1.0f;
    for (auto& [id, lock] : active_) {
        float sim = cosine_similarity(embedding, lock.centroid);
        if (sim >= theta && sim > best_sim) {
            best_sim = sim;
            best = &lock;
        }
    }
    return best;
}
```

Returns the highest-similarity conflicting lock, consistent with the current behavior of `overlap_trace`. Returning the highest-similarity conflict (rather than the first found) is a deterministic tie-breaking policy that gives the most conservative serialization: if you must block, block behind the most semantically similar active write.

---

## 3. Raft Consensus Integration

### 3.1 Why Raft, and What We Actually Need

Raft is a consensus algorithm that allows a cluster of nodes to agree on an ordered sequence of log entries. In our case, each log entry represents a lock table operation: either `ACQUIRE(agent_id, embedding, theta)` or `RELEASE(agent_id)`.

We do not need full Raft persistence to disk (though adding it is straightforward). For a research/demo system, in-memory Raft state with a snapshot on restart is sufficient. The minimum Raft features we need are:

- **Leader election** with term numbers
- **Log replication** (AppendEntries RPC)
- **Commit after quorum** (majority acknowledgment)
- **Heartbeats** to maintain leader authority

We do NOT need at this stage:
- Log compaction / snapshotting (lock table is small; full replay on restart is fast)
- Membership changes (the cluster is static: three nodes)
- Linearizable reads from followers

### 3.2 Raft State Machine

Each `dscc-node` runs a `RaftNode` that manages:

```
Persistent state (survives restart — can be in-memory for now):
  currentTerm   int64    // latest term this node has seen
  votedFor      string   // node_id voted for in currentTerm (or "")
  log[]         LogEntry // the replicated operation log

Volatile state (all nodes):
  commitIndex   int64    // highest log index known to be committed
  lastApplied   int64    // highest log index applied to lock table

Volatile state (leader only):
  nextIndex[f]  int64    // next log index to send to follower f
  matchIndex[f] int64    // highest log index known replicated to follower f
```

A `LogEntry` contains:

```cpp
struct LogEntry {
    int64_t term;
    enum class OpType { ACQUIRE, RELEASE } op_type;
    std::string agent_id;
    std::vector<float> embedding;   // only for ACQUIRE
    float theta;                    // only for ACQUIRE
};
```

### 3.3 Raft Node States

```
         timeout / no heartbeat
FOLLOWER ─────────────────────────► CANDIDATE
    ▲                                   │
    │ discover higher term               │ receive majority votes
    │ or valid leader                    ▼
    └───────────────────────────── LEADER
                                        │
                                        │ sends heartbeats (empty AppendEntries)
                                        │ to all followers every HEARTBEAT_MS
```

State transitions:

- **Follower → Candidate:** Election timeout fires (random interval between `ELECTION_TIMEOUT_MIN_MS` and `ELECTION_TIMEOUT_MAX_MS`). Node increments `currentTerm`, votes for itself, sends `RequestVote` to all peers.
- **Candidate → Leader:** Receives votes from a majority (≥2 of 3 nodes, including self). Immediately sends AppendEntries heartbeat to all peers to establish authority.
- **Candidate → Follower:** Receives an AppendEntries from a node with term ≥ its own, or loses the vote. Reverts to follower for the new term.
- **Leader → Follower:** Receives any RPC with a higher term. Steps down immediately.

### 3.4 Election Safety

The two safety guarantees that Raft election relies on:

**At most one leader per term:** Each node votes at most once per term, and a candidate needs a majority. Since there are three nodes, at most one can get 2 votes.

**Log completeness:** A candidate cannot win election unless its log is at least as up-to-date as the majority. "Up-to-date" means: the candidate's last log entry has a higher term than the voter's, or equal term and equal or longer log. This is enforced in the `RequestVote` handler.

### 3.5 Timing Parameters

```
HEARTBEAT_MS          = 50    // leader sends heartbeat this often
ELECTION_TIMEOUT_MIN  = 150   // follower waits at least this long
ELECTION_TIMEOUT_MAX  = 300   // follower waits at most this long
```

The election timeout must be significantly larger than the heartbeat interval so followers do not trigger spurious elections when the network is healthy. A ratio of 3x–10x is standard; we use 3x–6x here.

The randomization of the timeout (uniform random between MIN and MAX) ensures that in most cases one follower fires before the others, wins the election, and begins sending heartbeats before the other followers also time out — preventing split votes.

---

## 4. Replication Message Protocol

### 4.1 New gRPC Service: RaftService

Inter-node Raft communication happens via a new gRPC service defined in `proto/dscc_raft.proto`. This is separate from `dscc.proto` (the agent-facing API) to keep the two concerns cleanly separated.

```protobuf
syntax = "proto3";
package dscc_raft;

// ─── Core Raft RPCs ──────────────────────────────────────────────────────────

service RaftService {
  rpc RequestVote    (VoteRequest)       returns (VoteResponse);
  rpc AppendEntries  (AppendRequest)     returns (AppendResponse);
  rpc InstallSnapshot(SnapshotRequest)   returns (SnapshotResponse);
  rpc GetLeader      (LeaderQuery)       returns (LeaderInfo);
}

// ─── RequestVote ─────────────────────────────────────────────────────────────

message VoteRequest {
  int64  term           = 1;  // candidate's term
  string candidate_id   = 2;  // candidate requesting vote
  int64  last_log_index = 3;  // index of candidate's last log entry
  int64  last_log_term  = 4;  // term of candidate's last log entry
}

message VoteResponse {
  int64 term         = 1;  // currentTerm, for candidate to update itself
  bool  vote_granted = 2;
}

// ─── AppendEntries (also used as heartbeat when entries is empty) ─────────────

message LogEntry {
  int64  term      = 1;
  OpType op_type   = 2;
  string agent_id  = 3;
  repeated float embedding = 4;
  float  theta     = 5;

  enum OpType {
    ACQUIRE = 0;
    RELEASE = 1;
  }
}

message AppendRequest {
  int64  term           = 1;  // leader's term
  string leader_id      = 2;  // so followers can redirect clients
  int64  prev_log_index = 3;  // index of log entry before new ones
  int64  prev_log_term  = 4;  // term of prev_log_index entry
  repeated LogEntry entries = 5;  // empty for heartbeat
  int64  leader_commit  = 6;  // leader's commitIndex
}

message AppendResponse {
  int64 term    = 1;   // currentTerm, for leader to update itself
  bool  success = 2;   // true if follower contained matching prev entry
  int64 match_index = 3;  // highest index follower now has (on success)
  // On failure, follower hints at the conflicting term to allow
  // fast log rollback (optimization, not required for correctness).
  int64 conflict_term  = 4;
  int64 conflict_index = 5;
}

// ─── InstallSnapshot (used when a follower is too far behind) ────────────────

message SnapshotRequest {
  int64  term              = 1;
  string leader_id         = 2;
  int64  last_included_index = 3;
  int64  last_included_term  = 4;
  bytes  data              = 5;  // serialized lock table snapshot
}

message SnapshotResponse {
  int64 term = 1;
}

// ─── Leader discovery (used by proxy) ────────────────────────────────────────

message LeaderQuery  {}
message LeaderInfo {
  string leader_id      = 1;  // e.g. "node-1"
  string leader_address = 2;  // e.g. "dscc-node-1:50051"
  int64  current_term   = 3;
  bool   is_leader      = 4;  // true if responding node is the leader
}
```

### 4.2 AppendEntries: The Core Replication Flow

When the leader processes an `AcquireGuard` from an agent, the full replication flow is:

```
Leader receives AcquireGuard from agent
  │
  ├─ Run conflict check on local lock table
  │    If conflict → block agent thread (per-lock wait queue)
  │    If no conflict → continue
  │
  ├─ Append ACQUIRE entry to local log (not yet committed)
  │
  ├─ Send AppendEntries(entries=[ACQUIRE ...]) to node-2 and node-3 in parallel
  │
  ├─ Wait for majority acknowledgment (need 1 of 2 followers, since leader counts)
  │
  ├─ Advance commitIndex
  │
  ├─ Apply ACQUIRE to local lock table (add to active_)
  │
  ├─ Write to Qdrant (now holding the semantic lock)
  │
  ├─ Append RELEASE entry to local log
  │
  ├─ Send AppendEntries(entries=[RELEASE ...]) to node-2 and node-3
  │
  ├─ Wait for majority acknowledgment
  │
  ├─ Advance commitIndex
  │
  ├─ Apply RELEASE to local lock table (remove from active_, notify waiters)
  │
  └─ Return AcquireResponse to agent
```

On the follower side, each AppendEntries handler:

```
Follower receives AppendEntries(term, leader_id, prev_log_index,
                                prev_log_term, entries, leader_commit)
  │
  ├─ If term < currentTerm: reply false (stale leader)
  │
  ├─ Reset election timeout (valid leader is alive)
  │
  ├─ If log[prev_log_index].term != prev_log_term:
  │    Reply false with conflict hint
  │
  ├─ For each entry in entries:
  │    If existing log entry at same index conflicts with new entry:
  │      Truncate log from that index onward
  │    Append entry
  │
  ├─ If leader_commit > commitIndex:
  │    commitIndex = min(leader_commit, last_new_entry_index)
  │
  ├─ Apply any newly committed entries to lock table
  │    (calls apply_acquire or apply_release on ActiveLockTable)
  │
  └─ Reply success with match_index
```

### 4.3 Quorum Semantics for Lock Operations

A critical design decision: **when does the leader consider a lock acquired?**

Option A: Apply immediately, replicate asynchronously. Faster, but if the leader crashes before replicating, the lock state is lost and followers diverge.

Option B: Replicate to quorum, then apply. Slower, but durable. If the leader crashes after a quorum acknowledges, any new leader will have the log entry and will apply it.

**We choose Option B.** For a lock manager, correctness is more important than latency. The semantic lock must be durable before the Qdrant write proceeds — otherwise a crash could result in a Qdrant write that no other node knows about, with no corresponding lock in the replicated table.

In practice with three nodes on a local Docker network, the AppendEntries round-trip is under 5ms, so this has minimal impact on observed latency.

### 4.4 Log Entry Batching

For the initial implementation, each `AcquireGuard` produces exactly two log entries: one ACQUIRE and one RELEASE. The leader sends them separately (ACQUIRE first, waits for quorum commit, does Qdrant write, then RELEASE). They are not batched.

A future optimization would be to pipeline log entries: the leader could append multiple entries in one AppendEntries call if they are queued, reducing round-trips. This is not required for correctness.

### 4.5 Snapshot Protocol

If a follower is significantly behind (e.g., it was down for a while), replaying the entire log may be impractical or the log may have been compacted. The `InstallSnapshot` RPC transfers a serialized snapshot of the entire lock table state.

The snapshot format is a simple serialized list of currently active locks:

```protobuf
message LockTableSnapshot {
  int64 last_included_index = 1;
  int64 last_included_term  = 2;
  repeated SnapshotLock active_locks = 3;
}

message SnapshotLock {
  string agent_id       = 1;
  repeated float vector = 2;
  float  theta          = 3;
}
```

For the initial implementation, snapshots are only needed if a follower's `nextIndex` has fallen behind a leader-side compaction boundary. Since we do not compact logs initially, `InstallSnapshot` can be stubbed out and enabled later.

---

## 5. Leader-Aware Proxy

### 5.1 Responsibilities

The proxy has exactly three jobs:

1. Know which node is the current leader.
2. Forward `AcquireGuard` and `ReleaseGuard` RPCs to the leader.
3. Detect when the leader changes and update routing.

It does **not** do load balancing across nodes for write operations. All write-path RPCs go to the leader. The proxy may round-robin `Ping` RPCs across all live nodes for health checking.

### 5.2 Implementation Options

**Option A: Custom Go gRPC proxy (recommended)**

A small Go program (200–300 lines) that acts as a gRPC reverse proxy. It polls the `GetLeader` RPC on any live node every `LEADER_POLL_MS` milliseconds (default 100ms) to discover the current leader, then uses a `grpc.ClientConn` to that leader for forwarding. Go's `google.golang.org/grpc/proxy` or direct connection reuse makes this straightforward.

This is recommended because it gives precise control over the leader-detection logic and is easy to build and containerize.

**Option B: Envoy with a custom control plane**

Envoy can be used as the proxy if combined with a small control plane (also in Go or Python) that monitors leader election and updates Envoy's route configuration via xDS. More operationally complex but production-grade.

**Option C: Client-side leader tracking**

No proxy at all — the `e2e_bench` and any other clients track the leader themselves. Each node returns a `NOT_LEADER` error with the current leader's address if it receives an `AcquireGuard` while a follower. The client retries against the correct leader. This is how etcd clients work.

For this project, **Option A** is the right choice. It keeps client code simple and is easy to containerize alongside the existing Docker Compose stack.

### 5.3 Proxy Leader Discovery

```
// Proxy startup
known_nodes = ["dscc-node-1:50051", "dscc-node-2:50052", "dscc-node-3:50053"]
current_leader = ""

// Background goroutine (Go) or thread (C++)
LOOP every LEADER_POLL_MS:
  FOR each node in known_nodes:
    response = GetLeader(node)
    IF response.is_leader:
      current_leader = node
      BREAK
    ELSE IF response.leader_address != "":
      current_leader = response.leader_address
      BREAK
  IF current_leader == "":
    log("WARN: no leader found, cluster may be electing")

// Request forwarding
ON incoming AcquireGuard:
  IF current_leader == "":
    return UNAVAILABLE (503 equivalent)
  forward to current_leader
  IF response is REDIRECT(new_leader):
    current_leader = new_leader
    retry once
```

### 5.4 Redirect Protocol

If an agent's RPC reaches a follower node directly (bypassing the proxy, or during a brief leader transition), the follower returns a specific gRPC status:

```
Status code: FAILED_PRECONDITION
Message: "NOT_LEADER"
Metadata: "leader-address" → "dscc-node-2:50052"
```

The proxy catches this and retries against the redirected address. This allows the system to recover gracefully even during leader transitions without dropping agent requests.

---

## 6. New Component: RaftNode

### 6.1 Class Interface

**File: `src/raft_node.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include "dscc_raft.grpc.pb.h"

enum class RaftState { FOLLOWER, CANDIDATE, LEADER };

// Callback invoked when a log entry is committed and ready to be applied
// to the state machine (the ActiveLockTable).
using ApplyCallback = std::function<void(const dscc_raft::LogEntry&)>;

class RaftNode {
public:
    RaftNode(std::string node_id,
             std::vector<std::string> peer_addresses,
             ApplyCallback on_commit);

    // Start background threads (heartbeat sender, election timer).
    void Start();

    // Gracefully stop all background threads.
    void Stop();

    // Propose a new log entry. Blocks until committed to quorum or timeout.
    // Returns false if this node is not the leader or if quorum is not
    // reached within deadline.
    bool Propose(const dscc_raft::LogEntry& entry,
                 std::chrono::milliseconds timeout);

    // gRPC handler implementations (called by RaftServiceImpl).
    dscc_raft::VoteResponse    HandleRequestVote   (const dscc_raft::VoteRequest&);
    dscc_raft::AppendResponse  HandleAppendEntries (const dscc_raft::AppendRequest&);
    dscc_raft::SnapshotResponse HandleInstallSnapshot(const dscc_raft::SnapshotRequest&);
    dscc_raft::LeaderInfo      HandleGetLeader     ();

    bool IsLeader() const;
    std::string LeaderAddress() const;
    int64_t CurrentTerm() const;

private:
    void ElectionTimerLoop();
    void HeartbeatLoop();
    void ReplicateToFollower(const std::string& peer_address);
    void AdvanceCommitIndex();
    void ApplyCommitted();
    void BecomeCandidate();
    void BecomeLeader();
    void BecomeFollower(int64_t term);
    int64_t RandomElectionTimeout();

    std::string node_id_;
    std::vector<std::string> peer_addresses_;
    ApplyCallback on_commit_;

    mutable std::mutex mu_;

    // Raft persistent state
    int64_t current_term_ = 0;
    std::string voted_for_;
    std::vector<dscc_raft::LogEntry> log_;  // 1-indexed (log_[0] is sentinel)

    // Raft volatile state
    int64_t commit_index_ = 0;
    int64_t last_applied_ = 0;
    RaftState state_ = RaftState::FOLLOWER;
    std::string current_leader_address_;

    // Leader volatile state
    std::unordered_map<std::string, int64_t> next_index_;
    std::unordered_map<std::string, int64_t> match_index_;

    // Synchronization
    std::condition_variable commit_cv_;  // woken when commit_index_ advances
    std::atomic<bool> running_ = false;
    std::chrono::steady_clock::time_point last_heartbeat_received_;

    std::thread election_timer_thread_;
    std::thread heartbeat_thread_;
    std::vector<std::thread> replication_threads_;

    // gRPC stubs to peers
    std::unordered_map<std::string,
        std::unique_ptr<dscc_raft::RaftService::Stub>> peer_stubs_;
};
```

### 6.2 Propose: How a Lock Operation Enters the Log

```cpp
bool RaftNode::Propose(const dscc_raft::LogEntry& entry,
                       std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mu_);

    if (state_ != RaftState::LEADER) {
        return false;
    }

    // Append to local log with current term.
    dscc_raft::LogEntry e = entry;
    e.set_term(current_term_);
    log_.push_back(e);
    int64_t proposed_index = log_.size() - 1;

    // Wake replication threads (they will pick up the new entry).
    // In this design, each follower has a dedicated replication thread that
    // loops and sends AppendEntries whenever nextIndex[f] <= last_log_index.
    // Releasing the lock here allows them to proceed.
    lock.unlock();

    // Wait until proposed_index is committed or timeout.
    lock.lock();
    bool committed = commit_cv_.wait_for(lock, timeout, [&]{
        return commit_index_ >= proposed_index;
    });

    return committed;
}
```

### 6.3 AppendEntries Handler (Follower Side)

```cpp
dscc_raft::AppendResponse RaftNode::HandleAppendEntries(
    const dscc_raft::AppendRequest& req)
{
    std::unique_lock lock(mu_);
    dscc_raft::AppendResponse resp;

    if (req.term() < current_term_) {
        resp.set_term(current_term_);
        resp.set_success(false);
        return resp;
    }

    // Valid leader — reset election timer.
    last_heartbeat_received_ = std::chrono::steady_clock::now();

    if (req.term() > current_term_) {
        BecomeFollower(req.term());
    }
    current_leader_address_ = req.leader_id(); // store for redirect

    // Consistency check.
    int64_t prev = req.prev_log_index();
    if (prev > 0 && (prev >= (int64_t)log_.size() ||
                     log_[prev].term() != req.prev_log_term())) {
        resp.set_term(current_term_);
        resp.set_success(false);
        // Provide conflict hint for fast rollback.
        if (prev < (int64_t)log_.size()) {
            resp.set_conflict_term(log_[prev].term());
            // Find first index of conflicting term.
            int64_t ci = prev;
            while (ci > 0 && log_[ci-1].term() == log_[prev].term()) --ci;
            resp.set_conflict_index(ci);
        } else {
            resp.set_conflict_index(log_.size());
        }
        return resp;
    }

    // Append new entries, truncating conflicting tail.
    for (int i = 0; i < req.entries_size(); ++i) {
        int64_t idx = prev + 1 + i;
        if (idx < (int64_t)log_.size()) {
            if (log_[idx].term() != req.entries(i).term()) {
                log_.resize(idx);  // truncate
            } else {
                continue;  // already have this entry
            }
        }
        log_.push_back(req.entries(i));
    }

    // Advance commit index.
    if (req.leader_commit() > commit_index_) {
        commit_index_ = std::min(req.leader_commit(),
                                 (int64_t)log_.size() - 1);
        commit_cv_.notify_all();
    }

    resp.set_term(current_term_);
    resp.set_success(true);
    resp.set_match_index(log_.size() - 1);
    return resp;
}
```

### 6.4 Commit Application Loop

A background thread (or inline in `HandleAppendEntries`) applies committed log entries to the state machine. This calls the `ApplyCallback` (which calls `ActiveLockTable::apply_acquire` or `apply_release`).

```cpp
void RaftNode::ApplyCommitted() {
    // Runs in a dedicated thread.
    while (running_) {
        std::unique_lock lock(mu_);
        commit_cv_.wait(lock, [&]{
            return last_applied_ < commit_index_ || !running_;
        });
        while (last_applied_ < commit_index_) {
            ++last_applied_;
            dscc_raft::LogEntry entry = log_[last_applied_];
            lock.unlock();
            on_commit_(entry);  // applies to ActiveLockTable
            lock.lock();
        }
    }
}
```

---

## 7. Updated LockServiceImpl

### 7.1 New Constructor

`LockServiceImpl` now takes a `RaftNode*` in addition to its existing configuration:

```cpp
LockServiceImpl(RaftNode* raft,
                ActiveLockTable* lock_table,
                float theta,
                int lock_hold_ms,
                const std::string& qdrant_host,
                int qdrant_port,
                const std::string& qdrant_collection);
```

The `RaftNode` is owned by `main.cpp` and shared with `LockServiceImpl`. This allows the Raft inter-node service and the agent-facing service to share the same Raft state.

### 7.2 Updated AcquireGuard Flow

```cpp
grpc::Status LockServiceImpl::AcquireGuard(
    grpc::ServerContext* ctx,
    const dscc::AcquireRequest* req,
    dscc::AcquireResponse* resp)
{
    // 1. Validate inputs.
    if (req->agent_id().empty() || req->embedding_size() == 0) {
        return grpc::Status(grpc::INVALID_ARGUMENT, "agent_id and embedding required");
    }

    // 2. If not leader, redirect.
    if (!raft_->IsLeader()) {
        ctx->AddTrailingMetadata("leader-address", raft_->LeaderAddress());
        return grpc::Status(grpc::FAILED_PRECONDITION, "NOT_LEADER");
    }

    std::vector<float> embedding(req->embedding().begin(), req->embedding().end());
    auto t_received = now_ms();

    // 3. Acquire semantic lock (may block in per-lock wait queue).
    AcquireTrace trace = lock_table_->acquire(req->agent_id(), embedding, theta_);
    auto t_lock_acquired = now_ms();

    // 4. Propose ACQUIRE to Raft log. Must commit to quorum before proceeding.
    dscc_raft::LogEntry acquire_entry;
    acquire_entry.set_op_type(dscc_raft::LogEntry::ACQUIRE);
    acquire_entry.set_agent_id(req->agent_id());
    for (float f : embedding) acquire_entry.add_embedding(f);
    acquire_entry.set_theta(theta_);

    if (!raft_->Propose(acquire_entry, std::chrono::milliseconds(2000))) {
        // Raft quorum failed (e.g. too many nodes down). Release local lock
        // and report failure.
        lock_table_->release(req->agent_id());
        return grpc::Status(grpc::UNAVAILABLE, "Raft quorum not reached for ACQUIRE");
    }

    // 5. Write to Qdrant while holding the semantic lock.
    ScopeExit release_guard([&]{
        // 7. Propose RELEASE to Raft log.
        dscc_raft::LogEntry release_entry;
        release_entry.set_op_type(dscc_raft::LogEntry::RELEASE);
        release_entry.set_agent_id(req->agent_id());
        raft_->Propose(release_entry, std::chrono::milliseconds(2000));
        // Note: apply_release on the local table happens via on_commit_ callback.
    });

    bool qdrant_ok = upsert_embedding_to_qdrant(
        req->agent_id(), embedding,
        req->payload_text(), req->source_file(),
        req->timestamp_unix_ms());
    auto t_qdrant_done = now_ms();

    if (lock_hold_ms_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(lock_hold_ms_));
    }

    auto t_released = now_ms();

    // 8. Populate response.
    resp->set_granted(qdrant_ok);
    resp->set_server_received_unix_ms(t_received);
    resp->set_lock_acquired_unix_ms(t_lock_acquired);
    resp->set_qdrant_write_complete_unix_ms(t_qdrant_done);
    resp->set_lock_released_unix_ms(t_released);
    resp->set_lock_wait_ms(t_lock_acquired - t_received);
    resp->set_blocking_similarity_score(trace.blocking_similarity_score);
    resp->set_blocking_agent_id(trace.blocking_agent_id);

    return grpc::Status::OK;
}
```

**Important note on the acquire-then-propose ordering:** The local `lock_table_->acquire()` happens *before* the Raft `Propose`. This might seem like a correctness issue — we've modified local state before getting quorum. However, note that `acquire()` on followers is driven by `apply_acquire()` via the commit callback, not directly. The leader's local table is authoritative for the conflict check; the Raft log is the mechanism by which that authoritative state is replicated. The ordering is: conflict check → local acquire → propose to quorum → Qdrant write → propose release to quorum → local release (via callback). If Raft quorum fails after local acquire, the `ScopeExit` guard issues a local release and returns UNAVAILABLE.

---

## 8. Updated Proto Contract

### 8.1 Changes to dscc.proto

The agent-facing proto (`proto/dscc.proto`) needs one addition: a new status code to handle the NOT_LEADER redirect. This is carried in gRPC trailing metadata (as described in §5.4), so no proto change is strictly required. However, adding a `leader_address` field to the response is cleaner:

```protobuf
message AcquireResponse {
  // ... existing fields unchanged ...
  bool   granted                    = 1;
  string message                    = 2;
  int64  server_received_unix_ms    = 3;
  int64  lock_acquired_unix_ms      = 4;
  int64  qdrant_write_complete_unix_ms = 5;
  int64  lock_released_unix_ms      = 6;
  int64  lock_wait_ms               = 7;
  float  blocking_similarity_score  = 8;
  string blocking_agent_id          = 9;

  // New: if granted=false because this node is not the leader,
  // leader_redirect contains the address to retry against.
  string leader_redirect            = 10;
  // New: which node processed this request (for debugging in multi-node runs).
  string serving_node_id            = 11;
}
```

### 8.2 New dscc_raft.proto

As defined in §4.1. This is a new file at `proto/dscc_raft.proto`.

---

## 9. Data Flow: Full Request Lifecycle

This section traces a complete `AcquireGuard` request through the new system, including replication.

```
t=0ms   Agent sends AcquireGuard to dscc-proxy:50050

t=1ms   Proxy determines current leader is dscc-node-1:50051
        Proxy forwards AcquireGuard to node-1

t=2ms   node-1 LockServiceImpl receives request
        node-1 verifies it is leader (raft_->IsLeader() == true)
        node-1 lock_table_.acquire("agent-A", vec_A, 0.78)
           → find_conflict returns nullptr (no active locks)
           → active_["agent-A"] = {vec_A, 0.78, {}}
           → returns immediately (no wait)

t=3ms   node-1 creates LogEntry{ACQUIRE, "agent-A", vec_A, 0.78}
        node-1 appends to local log at index 5

t=3ms   node-1 sends AppendEntries(term=2, prev=4, entries=[entry-5])
        to node-2 and node-3 in parallel

t=5ms   node-2 receives AppendEntries
        node-2 checks prev_log consistency (OK)
        node-2 appends entry-5 to local log
        node-2 calls apply_acquire("agent-A", vec_A, 0.78)
          → active_["agent-A"] inserted on node-2 (no blocking — follower)
        node-2 replies AppendResponse{success=true, match_index=5}

t=5ms   node-3 receives AppendEntries (similar to node-2)
        node-3 replies AppendResponse{success=true, match_index=5}

t=6ms   node-1 receives majority (both followers acknowledged)
        node-1 advances commit_index to 5
        node-1 ApplyCommitted thread wakes, calls on_commit_(entry-5)
          → apply_acquire on node-1's table (already applied above? see note)

t=6ms   node-1 proceeds to Qdrant write
        upsert_embedding_to_qdrant("agent-A", vec_A, ...)

t=50ms  Qdrant upsert returns success

t=50ms  node-1 sleeps LOCK_HOLD_MS (750ms)

t=800ms node-1 creates LogEntry{RELEASE, "agent-A"}
        node-1 appends to local log at index 6
        node-1 sends AppendEntries(entries=[entry-6]) to node-2, node-3

t=802ms node-2, node-3 apply RELEASE: active_.erase("agent-A")
        node-2, node-3 reply success

t=803ms node-1 quorum reached for entry-6
        node-1 applies RELEASE: active_.erase("agent-A"), notify waiters

t=803ms node-1 returns AcquireResponse{granted=true, lock_wait_ms=1, ...}
        to proxy, proxy forwards to agent
```

**Note on double-apply:** In the flow above, the leader applies the operation to the local lock table both in `LockServiceImpl::AcquireGuard` (step at t=2ms) and via the `on_commit_` callback (step at t=6ms). To avoid double-apply, the leader should either skip the callback for entries it proposed (by tagging them) or use the `apply_acquire`/`apply_release` path for all modifications and have `LockServiceImpl` wait for the commit callback before proceeding. The cleanest design: `LockServiceImpl` calls `lock_table_.acquire()` for the blocking wait (which is leader-only behavior), and then waits for Raft commit before calling `lock_table_.apply_acquire()` to actually insert into `active_`. The separation is: `acquire()` does the conflict check and blocks, `apply_acquire()` does the insert.

---

## 10. Leader Election: Step-by-Step

### 10.1 Normal Operation (No Failures)

```
node-1 (LEADER, term=2)  node-2 (FOLLOWER)         node-3 (FOLLOWER)
    │                         │                          │
    ├──── heartbeat ──────────►                          │
    ├──── heartbeat ──────────────────────────────────── ►
    │                         │  reset election timer   │  reset election timer
    │  (repeats every 50ms)   │                          │
```

### 10.2 Leader Failure and New Election

```
node-1 (LEADER, term=2)  node-2 (FOLLOWER)         node-3 (FOLLOWER)

node-1 crashes
                              │ election timeout fires  │
                              │ (e.g., 210ms)           │  (e.g., 280ms — fires later)
                              │                          │
                              │ BecomeCandidate()        │
                              │ currentTerm = 3          │
                              │ votedFor = "node-2"      │
                              │                          │
                              ├──── RequestVote(term=3, lastLogIndex=6, lastLogTerm=2) ──►
                              │                          │
                              │                          │ node-3 checks:
                              │                          │   term 3 > 2 → grant
                              │                          │   log is at least as up-to-date
                              │                          │   vote for node-2
                              │◄─── VoteResponse(term=3, granted=true) ─────────────────
                              │
                              │ received 2 votes (self + node-3) → majority
                              │ BecomeLeader()
                              │ currentTerm = 3
                              │
                              ├──── AppendEntries(term=3, entries=[]) ──────────────────►
                              │   (heartbeat to assert leadership)
                              │                          │  resets election timer
                              │
                              │ proxy polls GetLeader()
                              │ node-2 replies is_leader=true
                              │ proxy updates current_leader to node-2:50052
```

### 10.3 Network Partition (Split-Brain Prevention)

With three nodes, a network partition that isolates node-1 (the leader) from node-2 and node-3:

```
Partition A: {node-1}    Partition B: {node-2, node-3}
```

- node-1 continues to believe it is leader but cannot replicate to anyone. Any `Propose` calls will time out waiting for quorum (it needs 1 of 2 followers, and it has 0). So node-1 will refuse to grant locks — it cannot make forward progress.
- node-2 and node-3 hold an election, node-2 becomes leader in term 3.
- Agents are routed to node-2 via the proxy (which polls all nodes and finds node-2 is the leader).
- When the partition heals, node-1 receives an AppendEntries from node-2 with term=3 > its own term=2. node-1 steps down and becomes a follower.

This is Raft's core safety guarantee: at most one leader can make progress in any given term, and any leader that cannot reach a quorum cannot commit.

---

## 11. Failure Scenarios and Recovery

### 11.1 Leader Crashes Mid-Request (Before Quorum)

Scenario: node-1 receives `AcquireGuard`, appends to local log, sends AppendEntries to followers, then crashes before receiving their acknowledgment.

- node-1's log has the ACQUIRE entry at index N, but it was never committed (followers may or may not have it).
- A new leader (say node-2) is elected. If node-2 has the entry (received it before the crash), it will replicate and commit it in the new term. If not, the entry is lost from the log.
- The agent's gRPC call to node-1 will fail with a connection error. The proxy retries against the new leader. The agent sends a fresh `AcquireGuard`. This is safe — the original entry was never committed, so the lock was never formally acquired.
- Result: the agent's write may be retried and will succeed. No data corruption.

### 11.2 Leader Crashes After Quorum, Before Qdrant Write

Scenario: node-1 commits the ACQUIRE entry to quorum, then crashes before writing to Qdrant.

- The ACQUIRE is now durable in the Raft log on at least 2 of 3 nodes.
- New leader is elected. The new leader sees the committed ACQUIRE in its log and applies it to its lock table. The semantic lock for this agent is now held by the new leader.
- The agent's gRPC call is dropped. The agent retries. The proxy sends the retry to the new leader.
- The new leader runs `acquire()` for the agent. The agent is blocked behind its own previous lock (which the new leader holds from the replicated log).
- This is a problem: the agent is deadlocked waiting for its own lock to release, but the Qdrant write never happened, so the RELEASE will never come.
- **Mitigation:** Add an acquisition timeout (`ACQUIRE_TIMEOUT_MS`) to the per-lock wait queue. If an agent waits longer than this, it times out and returns failure. The lock entry for the crashed leader's agent will also need a lease/TTL (noted as future work). For now, a human operator can restart the node to flush the lock table.

### 11.3 Follower Crashes and Recovers

- The cluster continues to operate with 2 nodes (still has quorum).
- The recovering follower starts up, contacts the leader via heartbeat.
- The leader sends AppendEntries to catch the follower up from `nextIndex[follower]`.
- If the follower is too far behind (log compacted), the leader sends `InstallSnapshot`.
- The follower resumes normal operation.

### 11.4 Proxy Crashes

- Agents cannot reach the cluster until the proxy is restarted.
- Nodes continue running; in-flight RPCs on the leader are unaffected.
- The proxy is stateless (its only state is the current leader address, which it rediscovers immediately on restart). Recovery is fast.

---

## 12. Updated Repository Structure

```
Distributed-Semantic-Lock/
├── CMakeLists.txt
├── docker-compose.yml                  # Updated: 3 dscc-nodes + proxy
├── docker/
│   ├── Dockerfile                      # Unchanged (used for all 3 nodes)
│   └── Dockerfile.proxy                # New: Go proxy container
├── proto/
│   ├── dscc.proto                      # Updated: leader_redirect, serving_node_id
│   ├── dscc_raft.proto                 # New: Raft inter-node protocol
│   └── [generated files]
├── src/
│   ├── main.cpp                        # Updated: starts RaftNode + both gRPC services
│   ├── raft_node.h                     # New: RaftNode class declaration
│   ├── raft_node.cpp                   # New: Raft implementation
│   ├── raft_service_impl.h             # New: gRPC handler for RaftService
│   ├── raft_service_impl.cpp           # New: delegates to RaftNode
│   ├── lock_service_impl.h             # Updated: takes RaftNode*
│   ├── lock_service_impl.cpp           # Updated: Propose before Qdrant write
│   ├── active_lock_table.h             # Updated: per-lock wait queues
│   ├── active_lock_table.cpp           # Updated: new acquire/release logic
│   ├── threadsafe_log.h                # Unchanged
│   ├── threadsafe_log.cpp              # Unchanged
│   ├── testbench.cpp                   # Updated: test per-lock wait queues
│   └── e2e_bench.cpp                   # Updated: 3-node cluster scenarios
├── proxy/
│   ├── main.go                         # New: leader-aware gRPC proxy
│   ├── go.mod
│   └── go.sum
├── demo_inputs/
│   └── [unchanged]
└── [other files unchanged]
```

---

## 13. Updated Docker Compose Stack

```yaml
version: "3.8"

services:

  qdrant:
    image: qdrant/qdrant
    ports:
      - "6333:6333"

  embedding-service:
    image: ${EMBEDDING_IMAGE:-ollama/ollama:latest}
    ports:
      - "7997:11434"
    volumes:
      - ./.cache/ollama:/root/.ollama

  dscc-node-1:
    build:
      context: .
      dockerfile: docker/Dockerfile
    ports:
      - "50051:50051"
      - "50061:50061"          # Raft inter-node port
    environment:
      - NODE_ID=node-1
      - PORT=50051
      - RAFT_PORT=50061
      - PEERS=dscc-node-2:50062,dscc-node-3:50063
      - THETA=${DSCC_THETA:-0.78}
      - LOCK_HOLD_MS=${DSCC_LOCK_HOLD_MS:-750}
      - QDRANT_HOST=qdrant
      - QDRANT_PORT=6333
      - QDRANT_COLLECTION=${QDRANT_COLLECTION:-dscc_memory_e2e}
    depends_on:
      - qdrant

  dscc-node-2:
    build:
      context: .
      dockerfile: docker/Dockerfile
    ports:
      - "50052:50051"
      - "50062:50061"
    environment:
      - NODE_ID=node-2
      - PORT=50051
      - RAFT_PORT=50061
      - PEERS=dscc-node-1:50061,dscc-node-3:50063
      - THETA=${DSCC_THETA:-0.78}
      - LOCK_HOLD_MS=${DSCC_LOCK_HOLD_MS:-750}
      - QDRANT_HOST=qdrant
      - QDRANT_PORT=6333
      - QDRANT_COLLECTION=${QDRANT_COLLECTION:-dscc_memory_e2e}
    depends_on:
      - qdrant

  dscc-node-3:
    build:
      context: .
      dockerfile: docker/Dockerfile
    ports:
      - "50053:50051"
      - "50063:50061"
    environment:
      - NODE_ID=node-3
      - PORT=50051
      - RAFT_PORT=50061
      - PEERS=dscc-node-1:50061,dscc-node-2:50062
      - THETA=${DSCC_THETA:-0.78}
      - LOCK_HOLD_MS=${DSCC_LOCK_HOLD_MS:-750}
      - QDRANT_HOST=qdrant
      - QDRANT_PORT=6333
      - QDRANT_COLLECTION=${QDRANT_COLLECTION:-dscc_memory_e2e}
    depends_on:
      - qdrant

  dscc-proxy:
    build:
      context: ./proxy
      dockerfile: ../docker/Dockerfile.proxy
    ports:
      - "50050:50050"
    environment:
      - PROXY_PORT=50050
      - BACKEND_NODES=dscc-node-1:50051,dscc-node-2:50051,dscc-node-3:50051
      - LEADER_POLL_MS=100
    depends_on:
      - dscc-node-1
      - dscc-node-2
      - dscc-node-3
```

---

## 14. Build System Changes

### 14.1 New CMake Targets

| Target | New? | Sources | Notes |
|---|---|---|---|
| `dscc_proto` | Updated | `dscc.pb.cc`, `dscc.grpc.pb.cc`, `dscc_raft.pb.cc`, `dscc_raft.grpc.pb.cc` | Add raft proto stubs |
| `dscc-node` | Updated | + `raft_node.cpp`, `raft_service_impl.cpp` | Add Raft components |
| `dscc-testbench` | Updated | Same sources, updated test | New per-lock queue tests |
| `dscc-e2e-bench` | Updated | Same sources | 3-node scenarios |
| `dscc-raft-test` | New | `raft_node.cpp`, `raft_test.cpp` | In-process Raft correctness tests |

### 14.2 Proxy Build

The Go proxy is built separately from CMake. A `Makefile` in `proxy/` handles it:

```makefile
build:
    go build -o dscc-proxy ./main.go

docker:
    docker build -f ../docker/Dockerfile.proxy -t dscc-proxy .
```

---

## 15. Configuration Reference

### 15.1 dscc-node Environment Variables (Updated)

| Variable | Default | Purpose |
|---|---|---|
| `NODE_ID` | `node-1` | Unique identifier for this node in the Raft cluster |
| `PORT` | `50051` | gRPC port for agent-facing service |
| `RAFT_PORT` | `50061` | gRPC port for Raft inter-node communication |
| `PEERS` | `` | Comma-separated list of peer Raft addresses |
| `THETA` | `0.85` | Cosine similarity conflict threshold |
| `LOCK_HOLD_MS` | `0` | Milliseconds to hold lock after Qdrant write |
| `QDRANT_HOST` | `qdrant` | Qdrant hostname |
| `QDRANT_PORT` | `6333` | Qdrant port |
| `QDRANT_COLLECTION` | `dscc_memory` | Qdrant collection name |
| `ELECTION_TIMEOUT_MIN_MS` | `150` | Minimum election timeout |
| `ELECTION_TIMEOUT_MAX_MS` | `300` | Maximum election timeout |
| `HEARTBEAT_MS` | `50` | Leader heartbeat interval |
| `RAFT_PROPOSE_TIMEOUT_MS` | `2000` | Max time to wait for Raft quorum on a Propose call |
| `ACQUIRE_TIMEOUT_MS` | `30000` | Max time an agent waits in the per-lock queue |

### 15.2 dscc-proxy Environment Variables

| Variable | Default | Purpose |
|---|---|---|
| `PROXY_PORT` | `50050` | Port the proxy listens on |
| `BACKEND_NODES` | `` | Comma-separated list of node addresses |
| `LEADER_POLL_MS` | `100` | How often the proxy polls for the current leader |
| `REQUEST_TIMEOUT_MS` | `35000` | Timeout for forwarded RPCs |

---

## 16. Implementation Roadmap

The implementation should proceed in the following phases, each independently testable before the next begins.

### Phase 1: Per-Lock Wait Queue (No Distributed Components)

**Scope:** `active_lock_table.h`, `active_lock_table.cpp`, `testbench.cpp`

**Changes:**
- Replace `std::condition_variable cv_` (global) with per-lock `std::deque<WaitQueueEntry>` waiters
- Implement `find_conflict()`, updated `acquire()`, updated `release()`
- Add `apply_acquire()` and `apply_release()` stubs (they just call the existing insert/erase logic)
- Update `testbench.cpp` with new scenarios:
  - Verify FIFO ordering within a conflict group
  - Verify non-conflicting agents are not delayed by unrelated waiter queues
  - Verify thundering herd is eliminated (only one agent woken per release)

**Validation:** All existing e2e bench scenarios must pass unchanged. The per-lock queue is a behavioral improvement; it should not change observable serialization order.

**Estimated complexity:** Medium. The data structure change is localized. The trickiest part is the waiter re-queuing on release (§2.4).

### Phase 2: Raft Node (No Lock Integration Yet)

**Scope:** `raft_node.h`, `raft_node.cpp`, `proto/dscc_raft.proto`, new `dscc-raft-test` target

**Changes:**
- Implement `RaftNode` with in-memory log, election timer, heartbeat, `RequestVote`, `AppendEntries`
- `Propose()` uses a no-op `ApplyCallback` for now
- Write `raft_test.cpp`: in-process test with three `RaftNode` instances connected over localhost gRPC
  - Test: stable leader heartbeat keeps followers from electing
  - Test: kill leader thread → one follower becomes leader within `ELECTION_TIMEOUT_MAX_MS + HEARTBEAT_MS`
  - Test: Propose to leader → committed on all three nodes
  - Test: follower behind by 10 entries → catches up via AppendEntries

**Validation:** All three in-process Raft tests pass. No changes to lock table, gRPC service, or e2e bench.

**Estimated complexity:** High. This is the most algorithmically complex component. Recommend consulting the Raft paper (Ongaro & Ousterhout, 2014) and the TLA+ spec for subtle edge cases.

### Phase 3: Raft + Lock Table Integration

**Scope:** `lock_service_impl.h/.cpp`, `raft_service_impl.h/.cpp`, `main.cpp`, updated `dscc_proto`

**Changes:**
- Wire `RaftNode` into `LockServiceImpl` (§7.1, §7.2)
- `on_commit_` callback calls `lock_table_.apply_acquire()` or `apply_release()`
- `main.cpp` starts two gRPC servers: one on `PORT` (agent-facing), one on `RAFT_PORT` (Raft inter-node)
- Add `NOT_LEADER` redirect in `AcquireGuard`
- Update proto as in §8.1

**Validation:** Run a single `dscc-node` as a single-node Raft cluster (no peers). It should immediately become leader and behave identically to the original system. All existing e2e bench scenarios must pass.

### Phase 4: Three-Node Cluster

**Scope:** `docker-compose.yml`, `e2e_bench.cpp`, `proxy/main.go`

**Changes:**
- Update `docker-compose.yml` with three nodes and proxy (§13)
- Implement Go proxy (§5.2)
- Update `e2e_bench.cpp` with cluster scenarios:
  - Scenario 5: All agents connect via proxy → same serialization as before
  - Scenario 6: Kill node-1 mid-run → new leader elected, agents complete successfully
  - Scenario 7: Kill and restart node-3 → it catches up and rejoins

**Validation:** All four original scenarios pass. New scenarios 5–7 pass with expected timing.

### Phase 5: Hardening (Future Work, not MVP)

- Persistent Raft log (write-ahead log to disk)
- Log compaction / snapshotting
- Acquisition timeouts and lease-based lock expiry
- Structured logging and metrics
- CI pipeline

---

## 17. Known Limitations and Future Work

The following limitations are carried forward from the original system or introduced by this design:

**No lock ownership enforcement.** Any node can release any agent's lock. This was a pre-existing issue and remains unfixed.

**No acquisition timeout in Phase 1.** An agent can wait indefinitely in the per-lock queue if a lock holder crashes before releasing. `ACQUIRE_TIMEOUT_MS` is defined in configuration but enforced only in Phase 3+ (via `cv_.wait_for` with timeout).

**Raft log is in-memory.** If all three nodes crash simultaneously, the log is lost. For the research context this is acceptable; production use requires a durable write-ahead log.

**Static cluster membership.** The three-node cluster is fixed. Adding or removing nodes requires a membership change protocol (Joint Consensus in Raft) which is not implemented.

**Qdrant is a single point of failure.** The lock manager is now highly available, but Qdrant is still a single instance. A production deployment would use Qdrant's own clustering features.

**The "Distributed" name is now partially earned.** This design implements genuine distributed consensus across three nodes, but it is still a single-cluster, single-region system. True geographic distribution with partition tolerance across data centers is a further step.

**Proxy is a SPOF.** The proxy process is still single-instance. For higher availability, run multiple proxy instances behind a traditional TCP load balancer (HAProxy, AWS NLB), which is safe since the proxy is stateless.