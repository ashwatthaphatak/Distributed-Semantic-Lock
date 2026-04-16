// Implements the in-memory semantic lock table.
// This is the DSLM blocking layer that decides whether a request must wait.
// lock_service_impl.cpp calls into this file before any Qdrant write happens.

#include "active_lock_table.h"
#include "threadsafe_log.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {

std::string format_similarity(float value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

}  // namespace

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

        std::ostringstream oss;
        oss << "[LOCK_QUEUE] agent=" << agent_id
            << " waiting_on=" << conflict.lock->agent_id
            << " similarity=" << format_similarity(conflict.similarity)
            << " queue_position=" << conflict.lock->waiters.size()
            << " theta=" << format_similarity(threshold);
        log_line(oss.str());

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

void ActiveLockTable::apply_acquire(const std::string& agent_id,
                                    const std::vector<float>& embedding,
                                    float threshold) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        apply_acquire_locked(agent_id, embedding, threshold);
    }
    print_active_locks();
}

void ActiveLockTable::apply_release(const std::string& agent_id) {
    release(agent_id);
}

size_t ActiveLockTable::size() const {
    return active_count();
}

size_t ActiveLockTable::active_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_.size();
}

std::vector<std::string> ActiveLockTable::active_agent_ids() const {
    std::vector<std::string> agent_ids;
    {
        std::lock_guard<std::mutex> lock(mu_);
        agent_ids.reserve(active_.size());
        for (const auto& [agent_id, entry] : active_) {
            (void)entry;
            agent_ids.push_back(agent_id);
        }
    }

    std::sort(agent_ids.begin(), agent_ids.end());
    return agent_ids;
}

void ActiveLockTable::print_active_locks() const {
    const std::vector<std::string> agent_ids = active_agent_ids();

    std::ostringstream oss;
    oss << "ActiveLocks: [";
    for (size_t i = 0; i < agent_ids.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << agent_ids[i];
    }
    oss << "]";

    log_line(oss.str());
}

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

std::deque<std::shared_ptr<WaitQueueEntry>> ActiveLockTable::remove_lock_locked(
    const std::string& agent_id,
    bool& removed) {
    auto it = active_.find(agent_id);
    if (it == active_.end()) {
        removed = false;
        return {};
    }

    removed = true;
    std::deque<std::shared_ptr<WaitQueueEntry>> waiters = std::move(it->second.waiters);
    active_.erase(it);
    return waiters;
}

void ActiveLockTable::rebalance_waiters_locked(
    std::deque<std::shared_ptr<WaitQueueEntry>> waiters,
    std::vector<std::shared_ptr<WaitQueueEntry>>& granted_waiters) {
    while (!waiters.empty()) {
        std::shared_ptr<WaitQueueEntry> waiter = waiters.front();
        waiters.pop_front();

        const ConflictTrace conflict =
            find_conflict_locked(waiter->embedding, waiter->theta);
        if (conflict.lock != nullptr) {
            // The original blocker is gone, but a different active semantic
            // region still conflicts, so move the waiter to that lock's queue.
            waiter->ready = false;
            waiter->granted = false;
            ++waiter->queue_hops;
            conflict.lock->waiters.push_back(waiter);

            std::ostringstream oss;
            oss << "[LOCK_REQUEUE] agent=" << waiter->waiting_agent_id
                << " waiting_on=" << conflict.lock->agent_id
                << " similarity=" << format_similarity(conflict.similarity)
                << " queue_position=" << conflict.lock->waiters.size()
                << " queue_hops=" << waiter->queue_hops
                << " theta=" << format_similarity(waiter->theta);
            log_line(oss.str());
            continue;
        }

        apply_acquire_locked(waiter->waiting_agent_id,
                             waiter->embedding,
                             waiter->theta);
        waiter->granted = true;
        waiter->ready = true;

        std::ostringstream oss;
        oss << "[LOCK_GRANT] agent=" << waiter->waiting_agent_id
            << " queue_hops=" << waiter->queue_hops
            << " active_locks=" << active_.size();
        log_line(oss.str());
        granted_waiters.push_back(std::move(waiter));
    }
}

float ActiveLockTable::cosine_similarity(const std::vector<float>& a,
                                         const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) {
        return 0.0f;
    }

    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }

    if (norm_a <= 0.0 || norm_b <= 0.0) {
        return 0.0f;
    }

    const double similarity = dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
    const double clamped = std::max(-1.0, std::min(1.0, similarity));
    return static_cast<float>(clamped);
}
