// Declares the in-memory semantic lock table and the lock-acquire trace data.
// This is the core overlap-checking layer used by lock_service_impl.cpp.
// It owns the active lock list and exposes acquire/release operations.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct WaitQueueEntry {
    std::string waiting_agent_id;
    std::vector<float> embedding;
    float theta = 0.0f;
    std::shared_ptr<std::condition_variable> cv;
    // ready/granted split the wake-up path into "you may re-check now" vs
    // "the releaser already transferred ownership to you."
    bool ready = false;
    bool granted = false;
    // Counts how many times this waiter was moved to a different active lock's
    // queue before it finally reached the critical section.
    int queue_hops = 0;
};

struct SemanticLock {
    std::string agent_id;
    std::vector<float> centroid;
    float threshold;
    // Waiters are attached to the active lock that currently blocks them most.
    std::deque<std::shared_ptr<WaitQueueEntry>> waiters;
};

struct AcquireTrace {
    bool waited = false;
    float blocking_similarity_score = 0.0f;
    std::string blocking_agent_id;
    // wait_position is captured only for the first queue a request entered;
    // queue_hops captures any later requeueing.
    int wait_position = 0;
    int wake_count = 0;
    int queue_hops = 0;
};

class ActiveLockTable {
public:
    AcquireTrace acquire(const std::string& agent_id,
                         const std::vector<float>& embedding,
                         float threshold);

    void release(const std::string& agent_id);

    void apply_acquire(const std::string& agent_id,
                       const std::vector<float>& embedding,
                       float threshold);

    void apply_release(const std::string& agent_id);

    size_t size() const;
    size_t active_count() const;

    std::vector<std::string> active_agent_ids() const;

    static float cosine_similarity(const std::vector<float>& a,
                                   const std::vector<float>& b);

    void print_active_locks() const;

private:
    struct ConflictTrace {
        SemanticLock* lock = nullptr;
        float similarity = 0.0f;
    };

    ConflictTrace find_conflict_locked(const std::vector<float>& embedding,
                                       float threshold);

    void apply_acquire_locked(const std::string& agent_id,
                              const std::vector<float>& embedding,
                              float threshold);

    std::deque<std::shared_ptr<WaitQueueEntry>> remove_lock_locked(
        const std::string& agent_id,
        bool& removed);

    void rebalance_waiters_locked(
        std::deque<std::shared_ptr<WaitQueueEntry>> waiters,
        std::vector<std::shared_ptr<WaitQueueEntry>>& granted_waiters);

    // Active locks are keyed by agent id because release/apply-release events
    // arrive by id through both the local service path and the Raft callback.
    std::unordered_map<std::string, SemanticLock> active_;
    mutable std::mutex mu_;
};
