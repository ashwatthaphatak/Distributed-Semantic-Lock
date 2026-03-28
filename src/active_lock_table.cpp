// Implements the in-memory semantic lock table.
// This is the DSLM blocking layer that decides whether a request must wait.
// lock_service_impl.cpp calls into this file before any Qdrant write happens.

#include "active_lock_table.h"
#include "threadsafe_log.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

AcquireTrace ActiveLockTable::acquire(const std::string& agent_id,
                                      const std::vector<float>& embedding,
                                      float threshold) {
    std::unique_lock<std::mutex> lock(mu_);
    AcquireTrace aggregate_trace;
    while (true) {
        const AcquireTrace overlap = overlap_trace(embedding, threshold);
        if (!overlap.waited) {
            break;
        }

        aggregate_trace.waited = true;
        if (overlap.blocking_similarity_score >= aggregate_trace.blocking_similarity_score) {
            aggregate_trace.blocking_similarity_score = overlap.blocking_similarity_score;
            aggregate_trace.blocking_agent_id = overlap.blocking_agent_id;
        }

        std::ostringstream oss;
        oss << "[LOCK] " << agent_id
            << " blocked by " << overlap.blocking_agent_id
            << " similarity=" << std::fixed << std::setprecision(3)
            << overlap.blocking_similarity_score
            << " threshold=" << threshold;
        log_line(oss.str());
        cv_.wait(lock);
    }

    active_.push_back(SemanticLock{agent_id, embedding, threshold});
    lock.unlock();
    print_active_locks();
    return aggregate_trace;
}

void ActiveLockTable::release(const std::string& agent_id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = std::remove_if(active_.begin(), active_.end(),
                                 [&](const SemanticLock& entry) {
                                     return entry.agent_id == agent_id;
                                 });
        active_.erase(it, active_.end());
    }

    cv_.notify_all();
    print_active_locks();
}

size_t ActiveLockTable::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_.size();
}

void ActiveLockTable::print_active_locks() const {
    std::vector<std::string> agent_ids;
    {
        std::lock_guard<std::mutex> lock(mu_);
        agent_ids.reserve(active_.size());
        for (const auto& entry : active_) {
            agent_ids.push_back(entry.agent_id);
        }
    }

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

AcquireTrace ActiveLockTable::overlap_trace(const std::vector<float>& embedding,
                                            float threshold) {
    AcquireTrace trace;
    for (const auto& entry : active_) {
        const float similarity = cosine_similarity(embedding, entry.centroid);
        if (similarity >= threshold &&
            similarity >= trace.blocking_similarity_score) {
            trace.waited = true;
            trace.blocking_similarity_score = similarity;
            trace.blocking_agent_id = entry.agent_id;
        }
    }
    return trace;
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
