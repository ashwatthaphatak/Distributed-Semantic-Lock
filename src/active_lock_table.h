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

class ActiveLockTable {
public:
    void acquire(const std::string& agent_id,
                 const std::vector<float>& embedding,
                 float threshold);

    void release(const std::string& agent_id);

    size_t size() const;

    void print_active_locks() const;

private:
    bool overlap_exists(const std::vector<float>& embedding,
                        float threshold);

    float cosine_similarity(const std::vector<float>& a,
                            const std::vector<float>& b);

    std::vector<SemanticLock> active_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
};