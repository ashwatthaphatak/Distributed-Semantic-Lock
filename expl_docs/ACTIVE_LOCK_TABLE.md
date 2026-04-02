Here is a complete walkthrough of the `ActiveLockTable`, covering every path and edge case.

---

## The Core Data Model

There is a single global mutex `mu_` that protects **all** state. The active locks live in an `unordered_map` keyed by `agent_id`. Each `SemanticLock` in the map holds the lock owner's embedding (called `centroid`), the threshold `θ` that was used, and a **per-lock FIFO waiter queue** — a `deque<shared_ptr<WaitQueueEntry>>`.

Each `WaitQueueEntry` is a blocked thread that is sleeping on its own private `condition_variable`. This is the key design choice: instead of a single global CV that wakes everyone (thundering herd), each waiter has its own CV so only specific waiters are woken.

The two boolean flags on `WaitQueueEntry` — `ready` and `granted` — encode two distinct wakeup reasons, which is where most of the subtlety lives.

---

## acquire() — The Blocking Path

Here is what happens line by line when a thread calls `acquire()`:

```23:79:src/active_lock_table.cpp
AcquireTrace ActiveLockTable::acquire(const std::string& agent_id,
                                      const std::vector<float>& embedding,
                                      float threshold) {
    std::unique_lock<std::mutex> lock(mu_);
    AcquireTrace trace;
    bool granted_by_handoff = false;

    while (true) {
        const ConflictTrace conflict = find_conflict_locked(embedding, threshold);
        if (conflict.lock == nullptr) {
            apply_acquire_locked(agent_id, embedding, threshold);
            break;
        }

        std::shared_ptr<WaitQueueEntry> waiter = std::make_shared<WaitQueueEntry>();
        waiter->waiting_agent_id = agent_id;
        waiter->embedding = embedding;
        waiter->theta = threshold;
        waiter->cv = std::make_shared<std::condition_variable>();
        conflict.lock->waiters.push_back(waiter);

        trace.waited = true;
        if (conflict.similarity >= trace.blocking_similarity_score) {
            trace.blocking_similarity_score = conflict.similarity;
            trace.blocking_agent_id = conflict.lock->agent_id;
        }
        if (trace.wait_position == 0) {
            trace.wait_position = static_cast<int>(conflict.lock->waiters.size());
        }

        // ... log ...

        waiter->cv->wait(lock, [&waiter]() { return waiter->ready; });
        waiter->ready = false;
        ++trace.wake_count;
        trace.queue_hops = waiter->queue_hops;

        // A releaser can hand ownership to the waiter directly if rechecking
        // found no remaining conflict, so the waiter must not reacquire again.
        if (waiter->granted) {
            granted_by_handoff = true;
            break;
        }
    }

    lock.unlock();
    if (!granted_by_handoff) {
        print_active_locks();
    }
    return trace;
}
```

### Step-by-step:

**1. Lock `mu_` and enter the loop.**

**2. Conflict check (`find_conflict_locked`).** This scans every entry in `active_` and computes cosine similarity between the incoming embedding and each lock's centroid. If multiple active locks conflict (similarity >= threshold), it picks the one with the **highest** similarity. This is the "strongest blocker" heuristic — the waiter is attached to the lock that most semantically overlaps with it.

**3a. No conflict found** (`conflict.lock == nullptr`): The request is immediately granted. `apply_acquire_locked` inserts a new `SemanticLock` into `active_` keyed by this agent's ID. The loop breaks, the mutex is released, and the trace is returned.

**3b. Conflict found**: A new `WaitQueueEntry` is heap-allocated (via `shared_ptr`) and pushed to the **back** of the conflicting lock's waiter deque. This gives FIFO ordering within each lock's queue. The trace records the blocking agent and similarity.

**Important**: a fresh `WaitQueueEntry` is created on **every** iteration of the loop. If the waiter wakes up and finds it was NOT granted (the `else` path — see below), it goes back to the top of the loop, runs `find_conflict_locked` again, and if a conflict still exists, creates a *new* `WaitQueueEntry` with a *new* CV attached to a potentially different lock's queue. The old `WaitQueueEntry` is abandoned.

**4. `cv->wait()`** — This atomically releases `mu_` and puts the thread to sleep. The predicate is `waiter->ready`. The thread will only wake when someone sets `ready = true` and calls `notify_all()` on this specific waiter's CV.

**5. After waking**, `ready` is immediately set back to `false`, and the code checks `waiter->granted`:

- **`granted == true`** (ownership handoff): The releaser already inserted this agent into `active_` during rebalancing. The waiter must **not** re-run the loop or call `apply_acquire_locked` again — ownership was transferred. It breaks out immediately.

- **`granted == false`**: This path exists but in practice is never taken in the current code. Looking at the only two places that set `ready = true` — both are in `rebalance_waiters_locked`, and one always sets `granted = true` alongside it. The other path (requeue) sets `ready = false, granted = false` and does NOT notify. So in the current code, every wake-up is a grant handoff. However, the `while(true)` loop provides a safety net: if a waiter is somehow woken spuriously or by a future code path that sets `ready = true` without `granted = true`, it re-runs the conflict check from scratch.

---

## release() — The Trigger for Rebalancing

```81:99:src/active_lock_table.cpp
void ActiveLockTable::release(const std::string& agent_id) {
    bool removed = false;
    std::vector<std::shared_ptr<WaitQueueEntry>> granted_waiters;
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::deque<std::shared_ptr<WaitQueueEntry>> waiters =
            remove_lock_locked(agent_id, removed);
        if (!removed) {
            log_line("WARN: release called for unknown agent_id " + agent_id);
            return;
        }
        rebalance_waiters_locked(std::move(waiters), granted_waiters);
    }

    print_active_locks();
    for (const auto& waiter : granted_waiters) {
        waiter->cv->notify_all();
    }
}
```

**1. `remove_lock_locked`** erases the agent's entry from `active_` and returns the waiter deque that was attached to it. This is a move — the deque is now detached from any lock.

**2. `rebalance_waiters_locked`** processes the orphaned waiters (detailed below).

**3. Notifications happen AFTER `mu_` is released.** This is a critical design decision. All the rebalancing (which may move waiters to other locks' queues or grant them) happens entirely under `mu_`. Only after `mu_` is dropped are the granted waiters' CVs notified. This prevents a race where a granted waiter wakes up and tries to do something while rebalancing is still modifying the data structures.

### Edge case: releasing an unknown agent_id

If `release()` is called for an agent that isn't in `active_`, `remove_lock_locked` returns `removed = false` and an empty deque. The function logs a warning and returns. No rebalancing occurs. This can happen if the Raft apply callback fires a duplicate RELEASE (e.g., the leader releases locally and then the apply callback also calls `apply_release`).

---

## rebalance_waiters_locked() — The Core Logic

This is the most important function. It processes the waiter deque that was just detached from a released lock.

```203:244:src/active_lock_table.cpp
void ActiveLockTable::rebalance_waiters_locked(
    std::deque<std::shared_ptr<WaitQueueEntry>> waiters,
    std::vector<std::shared_ptr<WaitQueueEntry>>& granted_waiters) {
    while (!waiters.empty()) {
        std::shared_ptr<WaitQueueEntry> waiter = waiters.front();
        waiters.pop_front();

        const ConflictTrace conflict =
            find_conflict_locked(waiter->embedding, waiter->theta);
        if (conflict.lock != nullptr) {
            waiter->ready = false;
            waiter->granted = false;
            ++waiter->queue_hops;
            conflict.lock->waiters.push_back(waiter);
            // ... log [LOCK_REQUEUE] ...
            continue;
        }

        apply_acquire_locked(waiter->waiting_agent_id,
                             waiter->embedding,
                             waiter->theta);
        waiter->granted = true;
        waiter->ready = true;
        // ... log [LOCK_GRANT] ...
        granted_waiters.push_back(std::move(waiter));
    }
}
```

It pops waiters **front-to-back** (FIFO order) and for each one:

### Path A: Still conflicting (requeue)

`find_conflict_locked` checks the waiter's embedding against the **current** `active_` state — which may have changed because earlier iterations of this same loop may have granted other waiters (inserting them into `active_`). If a conflict is found:

- `ready` and `granted` are explicitly set to `false` (defensive — they should already be false).
- `queue_hops` is incremented. This tracks how many times this waiter was bounced from one lock's queue to another.
- The waiter is pushed to the **back** of the new conflicting lock's waiter deque.
- The waiter's CV is **not** notified. The thread remains asleep.
- The `WaitQueueEntry` `shared_ptr` is reused across hops — the sleeping thread in `acquire()` still holds its own `shared_ptr` to the same object and is waiting on the same CV.

### Path B: No conflict (grant via handoff)

- `apply_acquire_locked` inserts the waiter's agent into `active_` as a new lock holder.
- `granted = true` and `ready = true` are set on the `WaitQueueEntry`.
- The waiter is added to `granted_waiters` — the list of waiters whose CVs will be notified after `mu_` is released.

**When the sleeping thread wakes up** in `acquire()`, it sees `waiter->granted == true` and breaks out of the loop immediately without re-scanning for conflicts. This is safe because the rebalancer already verified no conflict existed and inserted the lock under `mu_` — the state hasn't changed.

---

## The Cascading Grant Effect

This is the most subtle edge case. Consider this scenario:

**Active locks:** Lock X (agent A), Lock Y (agent B)
**X's waiters:** [W1, W2]
**Y's waiters:** [W3]

Agent A releases lock X. Rebalancing begins on [W1, W2]:

1. **W1** is checked against `active_` (which now contains only Y). If W1 doesn't conflict with Y, W1 is **granted** — it's inserted into `active_`. Now `active_` = {Y, W1}.

2. **W2** is checked against `active_` = {Y, W1}. Notice that W1 was *just* granted in the previous iteration. If W2 conflicts with W1, W2 is **requeued** behind W1's lock. W2 now sits in W1's waiter deque, and `queue_hops` is incremented.

This means **granting one waiter can block subsequent waiters in the same rebalance batch**. The order matters. Because waiters are processed front-to-back, earlier waiters in the queue get priority — preserving FIFO fairness within the conflict group.

### The chain reaction case

If W1 is granted and W2 doesn't conflict with anyone, then W2 is also granted in the same rebalance call. Both are inserted into `active_`, both are added to `granted_waiters`, and both are notified after `mu_` is released. Multiple waiters can be granted from a single release.

### The full requeue case

If all waiters from the released lock conflict with some other active lock, they are all requeued. Nobody is granted. Nobody is notified. The sleeping threads stay asleep until the lock they were requeued behind is eventually released, triggering another rebalance.

---

## apply_acquire vs acquire — Two Paths Into the Table

There are two ways a lock gets inserted into `active_`:

### `acquire()` — Leader's local admission path

Called by `LockServiceImpl::AcquireGuard` on the leader. This is the **blocking** path: it runs `find_conflict_locked`, waits if needed, and inserts into `active_` when there's no conflict. The calling thread is blocked until the lock is granted.

### `apply_acquire()` — Raft apply callback path

Called by the `on_commit_` callback on every node (leader and followers) when a committed ACQUIRE log entry is applied. This calls `apply_acquire_locked` directly — **no conflict check, no blocking**. It simply inserts or updates the lock in `active_`.

```171:186:src/active_lock_table.cpp
void ActiveLockTable::apply_acquire_locked(const std::string& agent_id,
                                           const std::vector<float>& embedding,
                                           float threshold) {
    auto it = active_.find(agent_id);
    if (it != active_.end()) {
        it->second.centroid = embedding;
        it->second.threshold = threshold;
        return;
    }

    SemanticLock lock_entry;
    lock_entry.agent_id = agent_id;
    lock_entry.centroid = embedding;
    lock_entry.threshold = threshold;
    active_.emplace(agent_id, std::move(lock_entry));
}
```

**Edge case: duplicate insert.** If `agent_id` already exists in `active_` (e.g., the leader's local `acquire()` already inserted it, and then the Raft apply callback fires `apply_acquire` for the same agent), it doesn't create a second entry. It updates the centroid and threshold in place. The waiter deque is left untouched. This prevents double-insertion from the leader's "admit locally, then replicate" ordering.

Similarly, `apply_release()` just delegates to `release()`, which handles the "unknown agent_id" case gracefully (logs a warning, does nothing). So if the leader already released a lock locally and the Raft callback fires a second release, it's harmless.

---

## find_conflict_locked — Strongest-Blocker Selection

```155:169:src/active_lock_table.cpp
ActiveLockTable::ConflictTrace ActiveLockTable::find_conflict_locked(
    const std::vector<float>& embedding,
    float threshold) {
    ConflictTrace best;
    for (auto& [agent_id, entry] : active_) {
        (void)agent_id;
        const float similarity = cosine_similarity(embedding, entry.centroid);
        if (similarity >= threshold &&
            (best.lock == nullptr || similarity >= best.similarity)) {
            best.lock = &entry;
            best.similarity = similarity;
        }
    }
    return best;
}
```

This always picks the **highest similarity** conflict. If two active locks both conflict, the waiter is attached to the more semantically similar one. The `>=` in the tie-breaking condition (`similarity >= best.similarity`) means the last-seen lock wins ties, but since `unordered_map` iteration order is non-deterministic, tie-breaking is effectively arbitrary.

**Edge case: the incoming request uses a different `θ` than the active lock.** The threshold used is always the **incoming request's** `threshold`, not the active lock's `threshold`. This means if lock A was acquired with `θ = 0.90` and request B arrives with `θ = 0.70`, B could be blocked by A even though A itself would not have considered B a conflict at its own threshold. The conflict check is always from the perspective of the incoming request.

---

## Notification Safety — Why Notify Happens Outside mu_

When `release()` collects the `granted_waiters` list and then drops `mu_` before calling `notify_all()`, there's an important invariant being maintained:

1. Under `mu_`, the rebalancer sets `granted = true` and `ready = true` on the waiter, and inserts the waiter's agent into `active_`.
2. `mu_` is dropped.
3. `cv->notify_all()` fires.
4. The sleeping thread in `acquire()` wakes up, re-acquires `mu_` (because `cv->wait` was called with a `unique_lock`), sees `ready == true`, sees `granted == true`, and breaks out.

Between steps 2 and 4, another thread could acquire `mu_` and modify `active_`. But the waking thread doesn't care — it sees `granted == true` and doesn't re-check `active_`. The rebalancer already made the decision and inserted the lock. The waking thread just breaks out and returns.

If the notification happened *inside* `mu_`, the waking thread would immediately try to re-acquire `mu_` and block on the releaser's lock scope, which would work but would serialize unnecessarily.

---

## Summary of Edge Cases

| Scenario | What Happens |
|----------|-------------|
| No conflict at all | Immediate insert into `active_`, no waiting |
| Single conflict | Waiter queued behind strongest blocker, sleeps on per-waiter CV |
| Multiple conflicts | Waiter queued behind the **highest similarity** active lock |
| Release with no waiters | Lock removed from `active_`, nothing to rebalance |
| Release with waiters, all can proceed | All waiters granted in FIFO order, all CVs notified |
| Release with waiters, some conflict with other locks | Conflicting ones requeued (queue_hops++), non-conflicting ones granted |
| Earlier waiter granted blocks later waiter in same rebalance | Later waiter is requeued behind the just-granted waiter |
| Duplicate `apply_acquire` (Raft callback after local acquire) | Updates centroid/threshold in place, doesn't create second lock |
| Duplicate `apply_release` (Raft callback after local release) | Logs warning "unknown agent_id", no-op |
| Release for unknown agent | Logs warning, returns immediately |
| Waiter woken with `granted == false` | Re-enters the `while(true)` loop, re-runs `find_conflict_locked` from scratch |
| Different θ values across agents | Conflict check uses the **incoming** request's θ, not the active lock's |
| Tie in similarity across multiple active locks | Arbitrary (depends on `unordered_map` iteration order) |