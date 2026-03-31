// Declares the in-memory semantic lock table and the lock-acquire trace data.
// This is the core overlap-checking layer used by lock_service_impl.cpp.
// It owns the active lock list and exposes acquire/release operations.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

struct SemanticLock {
    std::string agent_id;
    std::vector<float> centroid;
    float threshold;
};

struct AcquireTrace {
    bool waited = false;
    float blocking_similarity_score = 0.0f;
    std::string blocking_agent_id;
};

class ActiveLockTable {
public:
    AcquireTrace acquire(const std::string& agent_id,
                         const std::vector<float>& embedding,
                         float threshold);

    void release(const std::string& agent_id);

    size_t size() const;

    void print_active_locks() const;

private:
    AcquireTrace overlap_trace(const std::vector<float>& embedding,
                               float threshold);

    float cosine_similarity(const std::vector<float>& a,
                            const std::vector<float>& b);

    std::vector<SemanticLock> active_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
};
