// Runs focused in-process concurrency tests for the ActiveLockTable.
// This validates fairness and wakeup behavior without Docker or gRPC.

#include "active_lock_table.h"
#include "threadsafe_log.h"

#include <algorithm>
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
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct AgentPlan {
    std::string agent_id;
    std::vector<float> embedding;
    int start_delay_ms = 0;
    int hold_ms = 0;
};

struct AgentInterval {
    std::string agent_id;
    int64_t acquire_start_us = 0;
    int64_t acquire_success_us = 0;
    int64_t release_call_us = 0;
    int64_t release_us = 0;
};

struct AgentResult {
    AgentInterval interval;
    AcquireTrace trace;
};

struct CaseExecution {
    std::vector<AgentResult> results;
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

bool any_overlap(const std::vector<AgentResult>& results) {
    for (size_t i = 0; i < results.size(); ++i) {
        for (size_t j = i + 1; j < results.size(); ++j) {
            if (intervals_overlap(results[i].interval, results[j].interval)) {
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

void print_case_header(const std::string& case_id,
                       const std::string& title,
                       const std::string& behavior_expectation,
                       const std::vector<AgentPlan>& plans,
                       float threshold) {
    log_line("------------------------------------------------------------");
    log_line(case_id + " - " + title);
    log_line("Expectation: " + behavior_expectation);
    {
        std::ostringstream oss;
        oss << "Theta threshold: " << std::fixed << std::setprecision(2) << threshold;
        log_line(oss.str());
    }
    log_line("Agent plans:");
    for (const auto& plan : plans) {
        std::ostringstream oss;
        oss << "  " << plan.agent_id
            << " start_delay=" << plan.start_delay_ms << "ms"
            << " hold=" << plan.hold_ms << "ms"
            << " embedding=" << format_embedding(plan.embedding);
        log_line(oss.str());
    }
    log_line("Event stream:");
}

void print_case_timeline(const std::string& case_id,
                         const std::vector<AgentResult>& results) {
    log_line(case_id + " timeline (milliseconds):");
    for (const auto& result : results) {
        std::ostringstream oss;
        const double hold_ms =
            us_to_ms(result.interval.release_us - result.interval.acquire_success_us);
        oss << "  " << result.interval.agent_id
            << " start=" << std::fixed << std::setprecision(3)
            << us_to_ms(result.interval.acquire_start_us)
            << ", granted=" << us_to_ms(result.interval.acquire_success_us)
            << ", release-call=" << us_to_ms(result.interval.release_call_us)
            << ", released=" << us_to_ms(result.interval.release_us)
            << ", hold=" << hold_ms
            << ", waited=" << (result.trace.waited ? "true" : "false")
            << ", wait-position=" << result.trace.wait_position
            << ", wake-count=" << result.trace.wake_count
            << ", queue-hops=" << result.trace.queue_hops;
        log_line(oss.str());
    }
}

std::vector<std::string> acquisition_order(const std::vector<AgentResult>& results) {
    std::vector<std::pair<int64_t, std::string>> order;
    order.reserve(results.size());
    for (const auto& result : results) {
        order.emplace_back(result.interval.acquire_success_us, result.interval.agent_id);
    }
    std::sort(order.begin(), order.end());

    std::vector<std::string> ids;
    ids.reserve(order.size());
    for (const auto& [time_us, agent_id] : order) {
        (void)time_us;
        ids.push_back(agent_id);
    }
    return ids;
}

const AgentResult* find_agent(const std::vector<AgentResult>& results,
                              const std::string& agent_id) {
    for (const auto& result : results) {
        if (result.interval.agent_id == agent_id) {
            return &result;
        }
    }
    return nullptr;
}

CaseExecution run_case(const std::string& case_name,
                       const std::string& case_title,
                       const std::string& expectation_text,
                       const std::vector<AgentPlan>& plans,
                       float threshold) {
    ActiveLockTable table;
    CaseExecution execution;
    execution.results.resize(plans.size());
    std::vector<std::thread> threads;
    threads.reserve(plans.size());
    std::atomic<size_t> max_active{0};

    std::mutex start_mu;
    std::condition_variable ready_cv;
    std::condition_variable go_cv;
    size_t ready = 0;
    bool go = false;

    const auto case_start = Clock::now();
    Clock::time_point release_time = case_start;
    const auto now_us = [&]() -> int64_t {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   Clock::now() - case_start)
            .count();
    };

    print_case_header(case_name, case_title, expectation_text, plans, threshold);

    for (size_t i = 0; i < plans.size(); ++i) {
        threads.emplace_back([&, i]() {
            const AgentPlan plan = plans[i];
            {
                std::unique_lock<std::mutex> lock(start_mu);
                ++ready;
                if (ready == plans.size()) {
                    ready_cv.notify_one();
                }
                go_cv.wait(lock, [&]() { return go; });
            }

            std::this_thread::sleep_until(release_time +
                                          std::chrono::milliseconds(plan.start_delay_ms));

            execution.results[i].interval.agent_id = plan.agent_id;
            execution.results[i].interval.acquire_start_us = now_us();
            {
                std::ostringstream oss;
                oss << "[" << case_name << "][" << plan.agent_id << "] acquire-start @"
                    << std::fixed << std::setprecision(3)
                    << us_to_ms(execution.results[i].interval.acquire_start_us) << "ms";
                log_line(oss.str());
            }

            execution.results[i].trace =
                table.acquire(plan.agent_id, plan.embedding, threshold);

            execution.results[i].interval.acquire_success_us = now_us();
            const size_t active_now = table.size();
            update_max_active(max_active, active_now);
            {
                std::ostringstream oss;
                oss << "[" << case_name << "][" << plan.agent_id << "] acquire-granted @"
                    << std::fixed << std::setprecision(3)
                    << us_to_ms(execution.results[i].interval.acquire_success_us)
                    << "ms (active=" << active_now
                    << ", wakes=" << execution.results[i].trace.wake_count
                    << ", hops=" << execution.results[i].trace.queue_hops
                    << ")";
                log_line(oss.str());
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(plan.hold_ms));

            execution.results[i].interval.release_call_us = now_us();
            table.release(plan.agent_id);

            execution.results[i].interval.release_us = now_us();
            {
                std::ostringstream oss;
                oss << "[" << case_name << "][" << plan.agent_id << "] released @"
                    << std::fixed << std::setprecision(3)
                    << us_to_ms(execution.results[i].interval.release_us) << "ms";
                log_line(oss.str());
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(start_mu);
        ready_cv.wait(lock, [&]() { return ready == plans.size(); });
        release_time = Clock::now();
        go = true;
    }
    go_cv.notify_all();

    for (auto& thread : threads) {
        thread.join();
    }

    execution.max_active = max_active.load();
    execution.has_overlap = any_overlap(execution.results);
    print_case_timeline(case_name, execution.results);
    log_line("");
    return execution;
}

bool expect_true(const std::string& label, bool condition, bool& overall_pass) {
    if (condition) {
        log_line(label + ": PASS");
        return true;
    }
    log_line(label + ": FAIL");
    overall_pass = false;
    return false;
}

}  // namespace

int main() {
    constexpr float kTheta = 0.85f;

    std::vector<float> hot(8, 0.0f);
    hot[0] = 1.0f;

    std::vector<float> hot_variant = hot;
    hot_variant[1] = 0.01f;

    std::vector<float> hot_variant_two = hot;
    hot_variant_two[2] = 0.02f;

    std::vector<float> cold(8, 0.0f);
    cold[1] = 1.0f;

    std::vector<float> cold_variant = cold;
    cold_variant[2] = 0.01f;

    std::vector<AgentPlan> no_conflict_plans;
    for (int i = 0; i < 5; ++i) {
        std::vector<float> embedding(8, 0.0f);
        embedding[static_cast<size_t>(i)] = 1.0f;
        no_conflict_plans.push_back(
            AgentPlan{"independent-agent-" + std::to_string(i + 1), embedding, 0, 200});
    }

    std::vector<AgentPlan> conflict_plans;
    for (int i = 0; i < 5; ++i) {
        std::vector<float> embedding = hot;
        embedding[static_cast<size_t>((i % 3) + 1)] =
            0.001f * static_cast<float>(i + 1);
        conflict_plans.push_back(
            AgentPlan{"serialized-agent-" + std::to_string(i + 1), embedding, 0, 150});
    }

    const CaseExecution no_conflict = run_case(
        "Scenario-1",
        "Independent embeddings",
        "multiple agents should be active at the same time",
        no_conflict_plans,
        kTheta);

    const CaseExecution conflict = run_case(
        "Scenario-2",
        "Nearly identical embeddings",
        "only one agent should be active at a time",
        conflict_plans,
        kTheta);

    const CaseExecution fifo = run_case(
        "Scenario-3",
        "FIFO within a hot conflict group",
        "later arrivals should remain behind earlier waiters for the same region",
        {
            {"fifo-agent-1", hot, 0, 140},
            {"fifo-agent-2", hot_variant, 15, 90},
            {"fifo-agent-3", hot_variant_two, 30, 90},
        },
        kTheta);

    const CaseExecution hot_region = run_case(
        "Scenario-4",
        "Unrelated work bypasses a hot region",
        "a cold write should proceed while the hot queue remains serialized",
        {
            {"hot-holder", hot, 0, 220},
            {"hot-waiter-1", hot_variant, 20, 90},
            {"hot-waiter-2", hot_variant_two, 40, 90},
            {"cold-writer", cold, 60, 80},
        },
        kTheta);

    const CaseExecution isolated_wakes = run_case(
        "Scenario-5",
        "Wakeups stay local to the blocking region",
        "releasing one hot lock should not wake waiters blocked on another lock",
        {
            {"alpha-holder", hot, 0, 180},
            {"beta-holder", cold, 0, 320},
            {"alpha-waiter", hot_variant, 20, 60},
            {"beta-waiter", cold_variant, 20, 60},
        },
        kTheta);

    bool overall_pass = true;

    expect_true("Scenario-1 overlap", no_conflict.has_overlap, overall_pass);
    expect_true("Scenario-1 peak active > 1", no_conflict.max_active > 1, overall_pass);

    expect_true("Scenario-2 serialized", !conflict.has_overlap, overall_pass);
    expect_true("Scenario-2 peak active <= 1", conflict.max_active <= 1, overall_pass);

    const std::vector<std::string> fifo_order = acquisition_order(fifo.results);
    expect_true("Scenario-3 FIFO order",
                fifo_order ==
                    std::vector<std::string>{
                        "fifo-agent-1",
                        "fifo-agent-2",
                        "fifo-agent-3",
                    },
                overall_pass);
    const AgentResult* fifo_two = find_agent(fifo.results, "fifo-agent-2");
    const AgentResult* fifo_three = find_agent(fifo.results, "fifo-agent-3");
    expect_true("Scenario-3 waiter 2 queued first",
                fifo_two != nullptr && fifo_two->trace.wait_position == 1,
                overall_pass);
    expect_true("Scenario-3 waiter 3 queued second",
                fifo_three != nullptr && fifo_three->trace.wait_position == 2,
                overall_pass);

    const AgentResult* hot_holder = find_agent(hot_region.results, "hot-holder");
    const AgentResult* hot_waiter_one = find_agent(hot_region.results, "hot-waiter-1");
    const AgentResult* hot_waiter_two = find_agent(hot_region.results, "hot-waiter-2");
    const AgentResult* cold_writer = find_agent(hot_region.results, "cold-writer");
    expect_true("Scenario-4 cold writer not blocked",
                cold_writer != nullptr && !cold_writer->trace.waited,
                overall_pass);
    expect_true("Scenario-4 cold writer completes before hot holder releases",
                hot_holder != nullptr &&
                    cold_writer != nullptr &&
                    cold_writer->interval.acquire_success_us <
                        hot_holder->interval.release_call_us,
                overall_pass);
    expect_true("Scenario-4 hot queue still serialized",
                hot_waiter_one != nullptr &&
                    hot_waiter_two != nullptr &&
                    hot_waiter_one->interval.acquire_success_us <=
                        hot_waiter_two->interval.acquire_success_us,
                overall_pass);

    const AgentResult* alpha_waiter = find_agent(isolated_wakes.results, "alpha-waiter");
    const AgentResult* beta_waiter = find_agent(isolated_wakes.results, "beta-waiter");
    expect_true("Scenario-5 alpha waiter woke once",
                alpha_waiter != nullptr && alpha_waiter->trace.wake_count == 1,
                overall_pass);
    expect_true("Scenario-5 beta waiter woke once",
                beta_waiter != nullptr && beta_waiter->trace.wake_count == 1,
                overall_pass);
    expect_true("Scenario-5 alpha waiter progresses before beta waiter",
                alpha_waiter != nullptr &&
                    beta_waiter != nullptr &&
                    alpha_waiter->interval.acquire_success_us <
                        beta_waiter->interval.acquire_success_us,
                overall_pass);

    std::cout << "Final summary: " << (overall_pass ? "PASS" : "FAIL") << std::endl;
    return overall_pass ? 0 : 1;
}
