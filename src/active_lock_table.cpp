#include "active_lock_table.h"
#include "threadsafe_log.h"

#include <algorithm>
#include <cmath>
#include <sstream>

void ActiveLockTable::acquire(const std::string& agent_id,
                              const std::vector<float>& embedding,
                              float threshold) {
    std::unique_lock<std::mutex> lock(mu_);
    while (overlap_exists(embedding, threshold)) {
        cv_.wait(lock);
    }

    active_.push_back(SemanticLock{agent_id, embedding, threshold});
    lock.unlock();
    print_active_locks();
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

bool ActiveLockTable::overlap_exists(const std::vector<float>& embedding,
                                     float threshold) {
    for (const auto& entry : active_) {
        if (cosine_similarity(embedding, entry.centroid) >= threshold) {
            return true;
        }
    }
    return false;
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
