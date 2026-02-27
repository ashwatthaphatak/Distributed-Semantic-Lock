#include "active_lock_table.h"
#include "threadsafe_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct AgentInterval {
    std::string agent_id;
    int64_t acquire_start_us = 0;
    int64_t acquire_success_us = 0;
    int64_t release_call_us = 0;
    int64_t release_us = 0;
};

struct TestOutcome {
    bool pass = false;
    size_t max_active = 0;
    bool has_overlap = false;
};

double us_to_ms(int64_t micros) {
    return static_cast<double>(micros) / 1000.0;
}

bool intervals_overlap(const AgentInterval& a, const AgentInterval& b) {
    return a.acquire_success_us < b.release_call_us &&
           b.acquire_success_us < a.release_call_us;
}

bool any_overlap(const std::vector<AgentInterval>& intervals) {
    for (size_t i = 0; i < intervals.size(); ++i) {
        for (size_t j = i + 1; j < intervals.size(); ++j) {
            if (intervals_overlap(intervals[i], intervals[j])) {
                return true;
            }
        }
    }
    return false;
}

void update_max_active(std::atomic<size_t>& current_max, size_t value) {
    size_t observed = current_max.load();
    while (observed < value &&
           !current_max.compare_exchange_weak(observed, value)) {
    }
}

std::string format_embedding(const std::vector<float>& embedding) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < embedding.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << std::fixed << std::setprecision(2) << embedding[i];
    }
    oss << "]";
    return oss.str();
}

std::string make_agent_id(size_t index) {
    return "agent-" + std::to_string(index + 1);
}

void print_case_header(const std::string& case_id,
                       const std::string& title,
                       const std::string& behavior_expectation,
                       const std::vector<std::vector<float>>& embeddings,
                       float threshold) {
    log_line("------------------------------------------------------------");
    log_line(case_id + " - " + title);
    log_line("Expectation: " + behavior_expectation);
    {
        std::ostringstream oss;
        oss << "Theta threshold: " << std::fixed << std::setprecision(2) << threshold;
        log_line(oss.str());
    }
    log_line("Agent embeddings:");
    for (size_t i = 0; i < embeddings.size(); ++i) {
        log_line("  " + make_agent_id(i) + " -> " + format_embedding(embeddings[i]));
    }
    log_line("Event stream:");
}

void print_case_timeline(const std::string& case_id,
                         const std::vector<AgentInterval>& intervals) {
    log_line(case_id + " timeline (milliseconds):");
    for (const auto& interval : intervals) {
        std::ostringstream oss;
        const double hold_ms = us_to_ms(interval.release_us - interval.acquire_success_us);
        oss << "  " << interval.agent_id
            << " start=" << std::fixed << std::setprecision(3)
            << us_to_ms(interval.acquire_start_us)
            << ", granted=" << us_to_ms(interval.acquire_success_us)
            << ", release-call=" << us_to_ms(interval.release_call_us)
            << ", released=" << us_to_ms(interval.release_us)
            << ", hold=" << hold_ms;
        log_line(oss.str());
    }
}

TestOutcome run_case(const std::string& case_name,
                     const std::vector<std::vector<float>>& embeddings,
                     float threshold,
                     bool expect_serialized,
                     const std::string& case_title,
                     const std::string& expectation_text) {
    ActiveLockTable table;
    const size_t thread_count = embeddings.size();
    std::vector<AgentInterval> intervals(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    std::atomic<size_t> max_active{0};

    std::mutex start_mu;
    std::condition_variable ready_cv;
    std::condition_variable go_cv;
    size_t ready = 0;
    bool go = false;

    const auto case_start = Clock::now();
    const auto now_us = [&]() -> int64_t {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   Clock::now() - case_start)
            .count();
    };

    print_case_header(case_name, case_title, expectation_text, embeddings, threshold);

    for (size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back([&, i]() {
            const std::string agent_id = make_agent_id(i);
            auto acquire_guard = [&](const std::string& id,
                                     const std::vector<float>& embedding) {
                table.acquire(id, embedding, threshold);
            };
            auto release_guard = [&](const std::string& id) {
                table.release(id);
            };

            {
                std::unique_lock<std::mutex> lock(start_mu);
                ++ready;
                if (ready == thread_count) {
                    ready_cv.notify_one();
                }
                go_cv.wait(lock, [&]() { return go; });
            }

            intervals[i].agent_id = agent_id;
            intervals[i].acquire_start_us = now_us();
            {
                std::ostringstream oss;
                oss << "[" << case_name << "][" << agent_id << "] acquire-start @"
                    << std::fixed << std::setprecision(3)
                    << us_to_ms(intervals[i].acquire_start_us) << "ms";
                log_line(oss.str());
            }

            acquire_guard(agent_id, embeddings[i]);

            intervals[i].acquire_success_us = now_us();
            const size_t active_now = table.size();
            update_max_active(max_active, active_now);
            {
                std::ostringstream oss;
                oss << "[" << case_name << "][" << agent_id << "] acquire-granted @"
                    << std::fixed << std::setprecision(3)
                    << us_to_ms(intervals[i].acquire_success_us)
                    << "ms (active=" << active_now << ")";
                log_line(oss.str());
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            intervals[i].release_call_us = now_us();
            release_guard(agent_id);

            intervals[i].release_us = now_us();
            {
                std::ostringstream oss;
                oss << "[" << case_name << "][" << agent_id << "] released @"
                    << std::fixed << std::setprecision(3)
                    << us_to_ms(intervals[i].release_us) << "ms";
                log_line(oss.str());
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(start_mu);
        ready_cv.wait(lock, [&]() { return ready == thread_count; });
        go = true;
    }
    go_cv.notify_all();

    for (auto& thread : threads) {
        thread.join();
    }

    const bool overlap = any_overlap(intervals);
    const size_t peak = max_active.load();
    print_case_timeline(case_name, intervals);

    TestOutcome outcome;
    outcome.max_active = peak;
    outcome.has_overlap = overlap;
    if (expect_serialized) {
        outcome.pass = (peak <= 1) && !overlap;
    } else {
        outcome.pass = (peak > 1) && overlap;
    }
    {
        std::ostringstream oss;
        oss << case_name << " result: " << (outcome.pass ? "PASS" : "FAIL")
            << " (peak_active=" << outcome.max_active
            << ", overlap_detected=" << (outcome.has_overlap ? "true" : "false")
            << ")";
        log_line(oss.str());
    }
    log_line("");
    return outcome;
}

}  // namespace

int main() {
    constexpr size_t kThreads = 5;
    constexpr size_t kDim = 8;
    constexpr float kTheta = 0.85f;

    std::vector<std::vector<float>> no_conflict_embeddings(
        kThreads, std::vector<float>(kDim, 0.0f));
    for (size_t i = 0; i < kThreads; ++i) {
        no_conflict_embeddings[i][i] = 1.0f;
    }

    std::vector<std::vector<float>> conflict_embeddings(
        kThreads, std::vector<float>(kDim, 1.0f));
    for (size_t i = 0; i < kThreads; ++i) {
        conflict_embeddings[i][0] += static_cast<float>(i) * 0.0001f;
    }

    const TestOutcome test_a = run_case(
        "Scenario-1",
        no_conflict_embeddings,
        kTheta,
        false,
        "Independent embeddings (no semantic conflict)",
        "multiple agents should be active at the same time");

    const TestOutcome test_b = run_case(
        "Scenario-2",
        conflict_embeddings,
        kTheta,
        true,
        "Nearly identical embeddings (semantic conflict)",
        "only one agent should be active at a time");

    const bool overall_pass = test_a.pass && test_b.pass;
    std::cout << "Final summary: " << (overall_pass ? "PASS" : "FAIL") << std::endl;

    return overall_pass ? 0 : 1;
}
