// Author: Ayush Gala
// In-process Raft regression testbench: real localhost gRPC, three RaftNode peers.
// Covers election timing, quorum replication, leader failover, follower catch-up,
// AppendLocalEntry eventual replication, log truncation on conflict, and
// real ActiveLockTable integration through the apply callback.

#include "active_lock_table.h"
#include "raft_node.h"
#include "raft_service_impl.h"
#include "threadsafe_log.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
static int current_pid() {
    return static_cast<int>(_getpid());
}
#else
#include <unistd.h>
static int current_pid() {
    return static_cast<int>(::getpid());
}
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct AppliedRecord {
    std::vector<dscc_raft::LogEntry> entries;
    mutable std::mutex mu;
};

struct TestNode {
    std::string node_id;
    std::string service_address;
    std::string raft_address;
    AppliedRecord applied;
    std::unique_ptr<RaftNode> raft;
    std::unique_ptr<RaftServiceImpl> service;
    std::unique_ptr<grpc::Server> server;
};

// Per-process port block avoids collisions when multiple test binaries run locally.
// Range stays in unprivileged high ports; adjust if your environment reserves this band.
int port_block_base() {
    const int pid = current_pid();
    return 37100 + (pid % 400) * 20;
}

std::vector<std::string> raft_addresses_for_block(int base) {
    return {"127.0.0.1:" + std::to_string(base + 10),
            "127.0.0.1:" + std::to_string(base + 11),
            "127.0.0.1:" + std::to_string(base + 12)};
}

// Election timeouts must exceed (num_peers * rpc_timeout) so StartElection can
// finish collecting votes before the election timer fires again (see RAFT_EXPL §5.1).
RaftConfig test_config() {
    RaftConfig config;
    config.heartbeat_ms = 50;
    config.election_timeout_min_ms = 500;
    config.election_timeout_max_ms = 800;
    config.rpc_timeout_ms = 200;
    return config;
}

std::unique_ptr<TestNode> make_node(size_t index,
                                    const std::vector<std::string>& raft_addresses) {
    auto node = std::make_unique<TestNode>();
    node->node_id = "node-" + std::to_string(index + 1);
    node->service_address = "127.0.0.1:" + std::to_string(port_block_base() + static_cast<int>(index));
    node->raft_address = raft_addresses[index];

    std::vector<std::string> peers;
    for (size_t i = 0; i < raft_addresses.size(); ++i) {
        if (i != index) {
            peers.push_back(raft_addresses[i]);
        }
    }

    node->raft = std::make_unique<RaftNode>(
        node->node_id,
        node->service_address,
        peers,
        [record = &node->applied](const dscc_raft::LogEntry& entry) {
            std::lock_guard<std::mutex> lock(record->mu);
            record->entries.push_back(entry);
        },
        test_config());
    node->service = std::make_unique<RaftServiceImpl>(node->raft.get());

    grpc::ServerBuilder builder;
    builder.AddListeningPort(node->service_address, grpc::InsecureServerCredentials());
    builder.AddListeningPort(node->raft_address, grpc::InsecureServerCredentials());
    builder.RegisterService(node->service.get());
    node->server = builder.BuildAndStart();
    if (node->server == nullptr) {
        std::cerr << "[RAFT-TEST] BuildAndStart failed (service=" << node->service_address
                  << " raft=" << node->raft_address << ")" << std::endl;
        node->raft->Stop();
        return nullptr;
    }
    node->raft->Start();
    return node;
}

void stop_node(TestNode& node) {
    node.raft->Stop();
    if (node.server != nullptr) {
        node.server->Shutdown();
    }
}

void drain_after_shutdown() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

std::vector<dscc_raft::LogEntry> copy_applied(const TestNode& node) {
    std::lock_guard<std::mutex> lock(node.applied.mu);
    return node.applied.entries;
}

size_t applied_count(const TestNode& node) {
    std::lock_guard<std::mutex> lock(node.applied.mu);
    return node.applied.entries.size();
}

bool entries_vector_equal(const std::vector<dscc_raft::LogEntry>& a,
                          const std::vector<dscc_raft::LogEntry>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].SerializeAsString() != b[i].SerializeAsString()) {
            return false;
        }
    }
    return true;
}

int leader_index(const std::vector<std::unique_ptr<TestNode>>& nodes) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] != nullptr && nodes[i]->raft->IsLeader()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool wait_for_leader(const std::vector<std::unique_ptr<TestNode>>& nodes,
                     int* leader_out,
                     std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        int leader = -1;
        int leaders = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i] != nullptr && nodes[i]->raft->IsLeader()) {
                leader = static_cast<int>(i);
                ++leaders;
            }
        }
        if (leaders == 1) {
            *leader_out = leader;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool wait_for_applied_count(const TestNode& node,
                            size_t expected,
                            std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (applied_count(node) >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool wait_for_commit_index(const RaftNode& node,
                           int64_t min_index,
                           std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (node.CommitIndex() >= min_index) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool expect_true(const std::string& label, bool condition, bool& suite_ok) {
    log_line(std::string("[RAFT-TEST] ") + label + ": " + (condition ? "PASS" : "FAIL"));
    if (!condition) {
        suite_ok = false;
    }
    return condition;
}

dscc_raft::LogEntry make_acquire(const std::string& agent, float e0, float e1) {
    dscc_raft::LogEntry entry;
    entry.set_op_type(dscc_raft::LogEntry::ACQUIRE);
    entry.set_agent_id(agent);
    entry.add_embedding(e0);
    entry.add_embedding(e1);
    entry.set_theta(0.85f);
    return entry;
}

dscc_raft::LogEntry make_release(const std::string& agent) {
    dscc_raft::LogEntry entry;
    entry.set_op_type(dscc_raft::LogEntry::RELEASE);
    entry.set_agent_id(agent);
    return entry;
}

void run_scenario_basic_election_and_replication(std::vector<std::unique_ptr<TestNode>>& nodes,
                                                 bool& suite_ok) {
    int leader = -1;
    expect_true("S1: leader elected (3 nodes)",
                wait_for_leader(nodes, &leader, std::chrono::seconds(12)),
                suite_ok);
    if (leader < 0) {
        return;
    }

    const int stable = leader;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    expect_true("S1: leader unchanged after heartbeats",
                leader_index(nodes) == stable,
                suite_ok);

    int64_t idx = 0;
    const auto entry = make_acquire("s1-agent", 1.0f, 0.0f);
    expect_true("S1: Propose reaches quorum",
                nodes[static_cast<size_t>(leader)]->raft->Propose(
                    entry, std::chrono::seconds(5), &idx),
                suite_ok);
    expect_true("S1: WaitUntilApplied on leader",
                nodes[static_cast<size_t>(leader)]->raft->WaitUntilApplied(
                    idx, std::chrono::seconds(5)),
                suite_ok);

    for (size_t i = 0; i < nodes.size(); ++i) {
        expect_true("S1: follower/leader applied count >= 1 (node " + std::to_string(i + 1) + ")",
                    wait_for_applied_count(*nodes[i], 1, std::chrono::seconds(8)),
                    suite_ok);
    }

    const auto ref = copy_applied(*nodes[0]);
    for (size_t i = 1; i < nodes.size(); ++i) {
        expect_true("S1: applied log matches node-1 (node " + std::to_string(i + 1) + ")",
                    entries_vector_equal(ref, copy_applied(*nodes[i])),
                    suite_ok);
    }
}

void run_scenario_leader_failover(std::vector<std::unique_ptr<TestNode>>& nodes, bool& suite_ok) {
    int leader = -1;
    if (!wait_for_leader(nodes, &leader, std::chrono::seconds(12))) {
        expect_true("S2: initial leader", false, suite_ok);
        return;
    }

    stop_node(*nodes[static_cast<size_t>(leader)]);
    nodes[static_cast<size_t>(leader)].reset();
    drain_after_shutdown();

    int new_leader = -1;
    expect_true("S2: new leader after prior leader stopped",
                wait_for_leader(nodes, &new_leader, std::chrono::seconds(12)),
                suite_ok);
    if (new_leader < 0) {
        return;
    }

    int64_t idx = 0;
    const auto entry = make_acquire("s2-failover", 0.5f, 0.5f);
    expect_true("S2: Propose on replacement leader",
                nodes[static_cast<size_t>(new_leader)]->raft->Propose(
                    entry, std::chrono::seconds(6), &idx),
                suite_ok);
    expect_true("S2: applied on replacement leader",
                nodes[static_cast<size_t>(new_leader)]->raft->WaitUntilApplied(
                    idx, std::chrono::seconds(6)),
                suite_ok);

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] == nullptr) {
            continue;
        }
        expect_true("S2: live node caught entry (node " + std::to_string(i + 1) + ")",
                    wait_for_applied_count(*nodes[i], 1, std::chrono::seconds(8)),
                    suite_ok);
    }
}

void run_scenario_follower_outage_and_catchup(std::vector<std::unique_ptr<TestNode>>& nodes,
                                              const std::vector<std::string>& raft_addresses,
                                              bool& suite_ok) {
    int leader = -1;
    if (!wait_for_leader(nodes, &leader, std::chrono::seconds(12))) {
        expect_true("S3: initial leader", false, suite_ok);
        return;
    }

    size_t follower_to_stop = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (static_cast<int>(i) != leader) {
            follower_to_stop = i;
            break;
        }
    }

    stop_node(*nodes[follower_to_stop]);
    nodes[follower_to_stop].reset();
    drain_after_shutdown();

    constexpr int kBatch = 12;
    std::vector<int64_t> indices;
    indices.reserve(static_cast<size_t>(kBatch));
    for (int i = 0; i < kBatch; ++i) {
        dscc_raft::LogEntry e = make_release("catchup-" + std::to_string(i));
        int64_t idx = 0;
        const bool ok = nodes[static_cast<size_t>(leader)]->raft->Propose(
            e, std::chrono::seconds(6), &idx);
        if (!expect_true("S3: commit while follower down (" + std::to_string(i) + ")", ok, suite_ok)) {
            break;
        }
        indices.push_back(idx);
    }

    const int64_t last_index = indices.empty() ? 0 : indices.back();
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] == nullptr || static_cast<int>(i) == static_cast<int>(follower_to_stop)) {
            continue;
        }
        expect_true("S3: surviving peer reached commit index " + std::to_string(last_index),
                    wait_for_commit_index(*nodes[i]->raft, last_index, std::chrono::seconds(10)),
                    suite_ok);
    }

    nodes[follower_to_stop] = make_node(follower_to_stop, raft_addresses);
    expect_true("S3: restarted follower re-elected or synced (wait leader)",
                wait_for_leader(nodes, &leader, std::chrono::seconds(12)),
                suite_ok);

    expect_true("S3: restarted follower applied all " + std::to_string(kBatch) + " entries",
                wait_for_applied_count(*nodes[follower_to_stop], static_cast<size_t>(kBatch), std::chrono::seconds(15)),
                suite_ok);

    const auto ref = copy_applied(*nodes[static_cast<size_t>(leader)]);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] == nullptr) {
            continue;
        }
        expect_true("S3: applied vector matches leader after catch-up (node " + std::to_string(i + 1) + ")",
                    entries_vector_equal(ref, copy_applied(*nodes[i])),
                    suite_ok);
    }
}

void run_scenario_append_local_eventual(std::vector<std::unique_ptr<TestNode>>& nodes,
                                        bool& suite_ok) {
    int leader = -1;
    if (!wait_for_leader(nodes, &leader, std::chrono::seconds(12))) {
        expect_true("S4: initial leader", false, suite_ok);
        return;
    }

    const auto warmup = make_acquire("s4-warmup", 0.1f, 0.2f);
    int64_t warm_idx = 0;
    if (!nodes[static_cast<size_t>(leader)]->raft->Propose(warmup, std::chrono::seconds(6), &warm_idx)) {
        expect_true("S4: warmup propose", false, suite_ok);
        return;
    }
    for (auto& n : nodes) {
        wait_for_applied_count(*n, 1, std::chrono::seconds(10));
    }

    const auto ghost_release = make_release("s4-append-local-only");
    expect_true("S4: AppendLocalEntry accepted",
                nodes[static_cast<size_t>(leader)]->raft->AppendLocalEntry(ghost_release),
                suite_ok);

    int64_t tail = 0;
    {
        std::lock_guard<std::mutex> lock(nodes[static_cast<size_t>(leader)]->applied.mu);
        tail = static_cast<int64_t>(nodes[static_cast<size_t>(leader)]->applied.entries.size());
    }
    const int64_t want_applied = tail + 1;

    for (size_t i = 0; i < nodes.size(); ++i) {
        expect_true("S4: node " + std::to_string(i + 1) + " eventually applies AppendLocal entry",
                    wait_for_applied_count(*nodes[i], static_cast<size_t>(want_applied), std::chrono::seconds(15)),
                    suite_ok);
    }

    const auto ref = copy_applied(*nodes[0]);
    for (size_t i = 1; i < nodes.size(); ++i) {
        expect_true("S4: replicated apply stream matches (node " + std::to_string(i + 1) + ")",
                    entries_vector_equal(ref, copy_applied(*nodes[i])),
                    suite_ok);
    }
}

void run_scenario_acquire_release_chain(std::vector<std::unique_ptr<TestNode>>& nodes, bool& suite_ok) {
    int leader = -1;
    if (!wait_for_leader(nodes, &leader, std::chrono::seconds(12))) {
        expect_true("S5: initial leader", false, suite_ok);
        return;
    }

    auto& L = nodes[static_cast<size_t>(leader)]->raft;
    const std::string agent = "s5-chain-agent";
    int64_t a_idx = 0;
    int64_t r_idx = 0;
    expect_true("S5: ACQUIRE proposed",
                L->Propose(make_acquire(agent, 0.3f, 0.7f), std::chrono::seconds(6), &a_idx),
                suite_ok);
    expect_true("S5: ACQUIRE applied on leader", L->WaitUntilApplied(a_idx, std::chrono::seconds(6)), suite_ok);
    expect_true("S5: RELEASE proposed",
                L->Propose(make_release(agent), std::chrono::seconds(6), &r_idx),
                suite_ok);
    expect_true("S5: RELEASE applied on leader", L->WaitUntilApplied(r_idx, std::chrono::seconds(6)), suite_ok);

    for (size_t i = 0; i < nodes.size(); ++i) {
        expect_true("S5: two entries applied everywhere (node " + std::to_string(i + 1) + ")",
                    wait_for_applied_count(*nodes[i], 2, std::chrono::seconds(12)),
                    suite_ok);
    }
    const auto ref = copy_applied(*nodes[0]);
    for (size_t i = 1; i < nodes.size(); ++i) {
        expect_true("S5: lock-service-shaped log matches on all nodes",
                    entries_vector_equal(ref, copy_applied(*nodes[i])),
                    suite_ok);
    }
}

void run_scenario_many_entries_all_peers_up(std::vector<std::unique_ptr<TestNode>>& nodes,
                                            bool& suite_ok) {
    int leader = -1;
    if (!wait_for_leader(nodes, &leader, std::chrono::seconds(12))) {
        expect_true("S6: initial leader", false, suite_ok);
        return;
    }
    auto& L = nodes[static_cast<size_t>(leader)]->raft;

    constexpr int kN = 25;
    for (int i = 0; i < kN; ++i) {
        int64_t idx = 0;
        const bool ok =
            L->Propose(make_release("bulk-" + std::to_string(i)), std::chrono::seconds(8), &idx);
        if (!expect_true("S6: bulk propose " + std::to_string(i), ok, suite_ok)) {
            break;
        }
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        expect_true("S6: node " + std::to_string(i + 1) + " applied " + std::to_string(kN) + " bulk entries",
                    wait_for_applied_count(*nodes[i], static_cast<size_t>(kN), std::chrono::seconds(20)),
                    suite_ok);
    }
    const auto ref = copy_applied(*nodes[0]);
    for (size_t i = 1; i < nodes.size(); ++i) {
        expect_true("S6: bulk apply stream identical on node " + std::to_string(i + 1),
                    entries_vector_equal(ref, copy_applied(*nodes[i])),
                    suite_ok);
    }
}

// ---------------------------------------------------------------------------
// S8: Log truncation on conflicting entries.
//
// Raft correctness requires that when a new leader sends AppendEntries with
// entries at indices that already exist on a follower but with different terms,
// the follower must truncate its log from that point onward and accept the
// new entries.  This simulates the scenario:
//   1. Leader L1 commits an entry (warmup) to all three nodes.
//   2. A follower (F) is stopped.
//   3. L1 proposes entries that commit with only the remaining 2-node quorum.
//   4. L1 is stopped.  F is restarted.
//   5. A new leader L2 is elected at a higher term and proposes new entries
//      at potentially overlapping indices.
//   6. Verify all live nodes converge to the same log.
// ---------------------------------------------------------------------------
void run_scenario_log_truncation(std::vector<std::unique_ptr<TestNode>>& nodes,
                                 const std::vector<std::string>& raft_addresses,
                                 bool& suite_ok) {
    int leader = -1;
    if (!wait_for_leader(nodes, &leader, std::chrono::seconds(12))) {
        expect_true("S8: initial leader", false, suite_ok);
        return;
    }

    // Warmup: one entry committed on all three nodes.
    {
        int64_t idx = 0;
        expect_true("S8: warmup propose",
                    nodes[static_cast<size_t>(leader)]->raft->Propose(
                        make_acquire("s8-warmup", 0.1f, 0.9f),
                        std::chrono::seconds(6), &idx),
                    suite_ok);
        for (auto& n : nodes) {
            if (n) wait_for_applied_count(*n, 1, std::chrono::seconds(8));
        }
    }

    // Pick two distinct followers: one to isolate and one that stays with L1.
    std::vector<size_t> followers;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (static_cast<int>(i) != leader) followers.push_back(i);
    }
    const size_t follower_to_isolate = followers[0];
    const size_t follower_stays = followers[1];

    // Stop the follower we want to isolate.
    stop_node(*nodes[follower_to_isolate]);
    nodes[follower_to_isolate].reset();
    drain_after_shutdown();

    // L1 proposes entries while the isolated follower is down (quorum of 2).
    constexpr int kL1Entries = 3;
    for (int i = 0; i < kL1Entries; ++i) {
        int64_t idx = 0;
        if (!expect_true("S8: L1 propose while follower isolated (" +
                             std::to_string(i) + ")",
                         nodes[static_cast<size_t>(leader)]->raft->Propose(
                             make_release("s8-L1-entry-" + std::to_string(i)),
                             std::chrono::seconds(6), &idx),
                         suite_ok)) {
            return;
        }
    }

    const size_t old_leader = static_cast<size_t>(leader);

    // Stop L1 (old leader).
    stop_node(*nodes[old_leader]);
    nodes[old_leader].reset();
    drain_after_shutdown();

    // Restart the isolated follower — it has the warmup entry but NOT the
    // kL1Entries entries (or only a subset if any heartbeat got through).
    nodes[follower_to_isolate] = make_node(follower_to_isolate, raft_addresses);
    if (!expect_true("S8: isolated follower restarted",
                     nodes[follower_to_isolate] != nullptr, suite_ok)) {
        return;
    }

    // Wait for a new leader (L2) to be elected between the two live nodes.
    int new_leader = -1;
    expect_true("S8: new leader elected (L2)",
                wait_for_leader(nodes, &new_leader, std::chrono::seconds(15)),
                suite_ok);
    if (new_leader < 0) return;

    // L2 proposes new entries — these may end up at same indices that L1's
    // uncommitted entries (on the restarted follower) occupied, forcing
    // truncation on the follower with stale data.
    constexpr int kL2Entries = 2;
    for (int i = 0; i < kL2Entries; ++i) {
        int64_t idx = 0;
        if (!expect_true("S8: L2 propose (" + std::to_string(i) + ")",
                         nodes[static_cast<size_t>(new_leader)]->raft->Propose(
                             make_acquire("s8-L2-entry-" + std::to_string(i),
                                          0.5f, 0.5f),
                             std::chrono::seconds(6), &idx),
                         suite_ok)) {
            return;
        }
    }

    // Wait for both live nodes to apply the full log.
    const size_t expected_total = 1 + kL1Entries + kL2Entries;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i]) continue;
        expect_true("S8: node " + std::to_string(i + 1) +
                        " applied all " + std::to_string(expected_total),
                    wait_for_applied_count(*nodes[i], expected_total,
                                           std::chrono::seconds(15)),
                    suite_ok);
    }

    // Restart old leader and let it catch up — its log must be truncated
    // to match L2's authoritative log.
    nodes[old_leader] = make_node(old_leader, raft_addresses);
    if (nodes[old_leader]) {
        expect_true("S8: old leader catches up after restart",
                    wait_for_applied_count(*nodes[old_leader], expected_total,
                                           std::chrono::seconds(15)),
                    suite_ok);
    }

    // Final convergence check: every live node must have the same applied log.
    std::vector<dscc_raft::LogEntry> ref;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i]) { ref = copy_applied(*nodes[i]); break; }
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i]) continue;
        expect_true("S8: applied log matches across nodes (node " +
                        std::to_string(i + 1) + ")",
                    entries_vector_equal(ref, copy_applied(*nodes[i])),
                    suite_ok);
    }
}

// ---------------------------------------------------------------------------
// S9: Real ActiveLockTable wired through the Raft apply callback.
//
// Production uses an on_commit callback that calls apply_acquire / apply_release
// on an ActiveLockTable.  If that callback deadlocks or interacts badly with
// Raft's own mutex, the simple vector-append callback in S1–S8 would never
// catch it.  This test:
//   1. Constructs nodes with the production-shaped apply callback.
//   2. Proposes ACQUIRE entries through Raft and verifies every node's
//      ActiveLockTable reflects the acquired lock.
//   3. Starts a background thread that calls wait_for_admission (blocks on
//      the lock table condition variable) for a conflicting embedding.
//   4. Proposes a RELEASE through Raft, which triggers apply_release →
//      rebalance_waiters_locked → cv notify on every node, unblocking the
//      waiting thread.
//   5. Verifies the waiter was unblocked and the lock table state is correct
//      without any deadlocks.
// ---------------------------------------------------------------------------

struct LockTableTestNode {
    std::string node_id;
    std::string service_address;
    std::string raft_address;
    ActiveLockTable lock_table;
    AppliedRecord applied;
    std::unique_ptr<RaftNode> raft;
    std::unique_ptr<RaftServiceImpl> service;
    std::unique_ptr<grpc::Server> server;
};

std::unique_ptr<LockTableTestNode> make_lock_table_node(
    size_t index,
    const std::vector<std::string>& raft_addresses) {

    auto node = std::make_unique<LockTableTestNode>();
    node->node_id = "lt-node-" + std::to_string(index + 1);
    node->service_address =
        "127.0.0.1:" + std::to_string(port_block_base() + static_cast<int>(index));
    node->raft_address = raft_addresses[index];

    std::vector<std::string> peers;
    for (size_t i = 0; i < raft_addresses.size(); ++i) {
        if (i != index) peers.push_back(raft_addresses[i]);
    }

    ActiveLockTable* lt = &node->lock_table;
    AppliedRecord* record = &node->applied;
    node->raft = std::make_unique<RaftNode>(
        node->node_id,
        node->service_address,
        peers,
        [lt, record](const dscc_raft::LogEntry& entry) {
            if (entry.op_type() == dscc_raft::LogEntry::ACQUIRE) {
                std::vector<float> emb(entry.embedding().begin(),
                                       entry.embedding().end());
                lt->apply_acquire(entry.agent_id(), emb, entry.theta());
            } else {
                lt->apply_release(entry.agent_id());
            }
            {
                std::lock_guard<std::mutex> lock(record->mu);
                record->entries.push_back(entry);
            }
        },
        test_config());
    node->service = std::make_unique<RaftServiceImpl>(node->raft.get());

    grpc::ServerBuilder builder;
    builder.AddListeningPort(node->service_address,
                             grpc::InsecureServerCredentials());
    builder.AddListeningPort(node->raft_address,
                             grpc::InsecureServerCredentials());
    builder.RegisterService(node->service.get());
    node->server = builder.BuildAndStart();
    if (!node->server) {
        node->raft->Stop();
        return nullptr;
    }
    node->raft->Start();
    return node;
}

size_t lt_applied_count(const LockTableTestNode& node) {
    std::lock_guard<std::mutex> lock(node.applied.mu);
    return node.applied.entries.size();
}

bool wait_for_lt_applied_count(const LockTableTestNode& node,
                               size_t expected,
                               std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (lt_applied_count(node) >= expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int lt_leader_index(const std::vector<std::unique_ptr<LockTableTestNode>>& nodes) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] && nodes[i]->raft->IsLeader())
            return static_cast<int>(i);
    }
    return -1;
}

bool wait_for_lt_leader(
    const std::vector<std::unique_ptr<LockTableTestNode>>& nodes,
    int* leader_out,
    std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        int leader = -1;
        int leaders = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i] && nodes[i]->raft->IsLeader()) {
                leader = static_cast<int>(i);
                ++leaders;
            }
        }
        if (leaders == 1) {
            *leader_out = leader;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void run_scenario_real_lock_table(bool& suite_ok,
                                  const std::vector<std::string>& raft_addrs) {
    log_line("[RAFT-TEST] === S9 real ActiveLockTable via apply callback ===");

    std::vector<std::unique_ptr<LockTableTestNode>> nodes;
    for (size_t i = 0; i < 3; ++i) {
        nodes.push_back(make_lock_table_node(i, raft_addrs));
        if (!nodes.back()) {
            expect_true("S9: cluster bootstrap (node " + std::to_string(i + 1) +
                            ")", false, suite_ok);
            for (auto& n : nodes) {
                if (n) { n->raft->Stop(); n->server->Shutdown(); }
            }
            drain_after_shutdown();
            return;
        }
    }

    int leader = -1;
    if (!wait_for_lt_leader(nodes, &leader, std::chrono::seconds(12))) {
        expect_true("S9: leader elected", false, suite_ok);
        for (auto& n : nodes) {
            if (n) { n->raft->Stop(); n->server->Shutdown(); }
        }
        drain_after_shutdown();
        return;
    }
    auto& L = nodes[static_cast<size_t>(leader)]->raft;

    // Propose ACQUIRE through Raft.
    const std::string agent_a = "s9-agent-A";
    const std::vector<float> emb_a = {1.0f, 0.0f};
    int64_t a_idx = 0;
    expect_true("S9: ACQUIRE proposed",
                L->Propose(make_acquire(agent_a, emb_a[0], emb_a[1]),
                           std::chrono::seconds(6), &a_idx),
                suite_ok);
    expect_true("S9: ACQUIRE applied on leader",
                L->WaitUntilApplied(a_idx, std::chrono::seconds(6)),
                suite_ok);

    // Verify every node's lock table now holds agent_a.
    for (size_t i = 0; i < nodes.size(); ++i) {
        wait_for_lt_applied_count(*nodes[i], 1, std::chrono::seconds(8));
        const auto ids = nodes[i]->lock_table.active_agent_ids();
        bool found = false;
        for (const auto& id : ids) { if (id == agent_a) found = true; }
        expect_true("S9: lock table contains " + agent_a + " (node " +
                        std::to_string(i + 1) + ")", found, suite_ok);
    }

    // On the leader's lock table, start a background thread that blocks on
    // wait_for_admission with a conflicting embedding (cosine_similarity of
    // (1,0) and (0.95, 0.31) ≈ 0.95 > 0.85 threshold).
    ActiveLockTable& leader_lt =
        nodes[static_cast<size_t>(leader)]->lock_table;
    const std::string waiter_agent = "s9-waiter";
    const std::vector<float> emb_conflict = {0.95f, 0.31f};
    std::atomic<bool> waiter_unblocked{false};
    AcquireTrace waiter_trace;

    std::thread waiter_thread([&]() {
        waiter_trace = leader_lt.wait_for_admission(
            waiter_agent, emb_conflict, 0.85f);
        waiter_unblocked.store(true);
    });

    // Give the waiter thread time to block.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    expect_true("S9: waiter blocked while lock held",
                !waiter_unblocked.load(), suite_ok);

    // Propose RELEASE through Raft — this triggers apply_release on every
    // node, which calls release() → rebalance_waiters_locked → cv notify.
    int64_t r_idx = 0;
    expect_true("S9: RELEASE proposed",
                L->Propose(make_release(agent_a),
                           std::chrono::seconds(6), &r_idx),
                suite_ok);
    expect_true("S9: RELEASE applied on leader",
                L->WaitUntilApplied(r_idx, std::chrono::seconds(6)),
                suite_ok);

    // The apply_release on the leader's lock table should unblock the waiter.
    const auto waiter_deadline = Clock::now() + std::chrono::seconds(8);
    while (!waiter_unblocked.load() && Clock::now() < waiter_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect_true("S9: waiter unblocked after RELEASE applied",
                waiter_unblocked.load(), suite_ok);

    if (waiter_thread.joinable()) waiter_thread.join();

    expect_true("S9: waiter trace shows it waited", waiter_trace.waited, suite_ok);

    // The waiter was granted via wait_for_admission, so it should now be
    // a pending lock in the leader's table.  Clean it up.
    leader_lt.remove_pending(waiter_agent);

    // Verify all nodes applied both entries and agent_a is released.
    for (size_t i = 0; i < nodes.size(); ++i) {
        wait_for_lt_applied_count(*nodes[i], 2, std::chrono::seconds(8));
        const auto ids = nodes[i]->lock_table.active_agent_ids();
        bool agent_a_gone = true;
        for (const auto& id : ids) { if (id == agent_a) agent_a_gone = false; }
        expect_true("S9: " + agent_a + " released on node " +
                        std::to_string(i + 1), agent_a_gone, suite_ok);
    }

    for (auto& n : nodes) {
        if (n) { n->raft->Stop(); n->server->Shutdown(); }
    }
    drain_after_shutdown();
}

void run_scenario_no_split_brain_window(std::vector<std::unique_ptr<TestNode>>& nodes, bool& suite_ok) {
    int leader = -1;
    if (!wait_for_leader(nodes, &leader, std::chrono::seconds(12))) {
        expect_true("S7: initial leader for observation window", false, suite_ok);
        return;
    }
    (void)leader;
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (Clock::now() < deadline) {
        int c = 0;
        for (const auto& n : nodes) {
            if (n && n->raft->IsLeader()) {
                ++c;
            }
        }
        if (c > 1) {
            expect_true("S7: at most one leader in steady window", false, suite_ok);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    expect_true("S7: at most one leader in steady window", true, suite_ok);
}

template <typename Fn>
void run_named_suite(const char* title,
                     Fn fn,
                     bool& suite_ok,
                     const std::vector<std::string>& raft_addrs) {
    log_line(std::string("[RAFT-TEST] === ") + title + " ===");
    std::vector<std::unique_ptr<TestNode>> nodes;
    for (size_t i = 0; i < 3; ++i) {
        nodes.push_back(make_node(i, raft_addrs));
        if (!nodes.back()) {
            expect_true(std::string("cluster bootstrap (node ") + std::to_string(i + 1) + " listen ports)",
                        false,
                        suite_ok);
            for (auto& node : nodes) {
                if (node != nullptr) {
                    stop_node(*node);
                }
            }
            drain_after_shutdown();
            return;
        }
    }
    fn(nodes, suite_ok);
    for (auto& node : nodes) {
        if (node != nullptr) {
            stop_node(*node);
        }
    }
    drain_after_shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    bool suite_ok = true;
    const int base = port_block_base();
    const std::vector<std::string> raft_addrs = raft_addresses_for_block(base);

    log_line(std::string("[RAFT-TEST] port block base=") + std::to_string(base) +
             " raft=" + raft_addrs[0] + "," + raft_addrs[1] + "," + raft_addrs[2]);

    run_named_suite(
        "S1 election + basic replication", run_scenario_basic_election_and_replication, suite_ok, raft_addrs);
    run_named_suite("S2 leader failover", run_scenario_leader_failover, suite_ok, raft_addrs);
    run_named_suite(
        "S3 follower outage + cold restart catch-up",
        [&](std::vector<std::unique_ptr<TestNode>>& nodes, bool& ok) {
            run_scenario_follower_outage_and_catchup(nodes, raft_addrs, ok);
        },
        suite_ok,
        raft_addrs);
    run_named_suite(
        "S4 AppendLocalEntry eventual replication", run_scenario_append_local_eventual, suite_ok, raft_addrs);
    run_named_suite(
        "S5 ACQUIRE then RELEASE chain", run_scenario_acquire_release_chain, suite_ok, raft_addrs);
    run_named_suite(
        "S6 many entries all peers up", run_scenario_many_entries_all_peers_up, suite_ok, raft_addrs);
    run_named_suite(
        "S7 split-brain spot check", run_scenario_no_split_brain_window, suite_ok, raft_addrs);
    run_named_suite(
        "S8 log truncation on conflicting entries",
        [&](std::vector<std::unique_ptr<TestNode>>& nodes, bool& ok) {
            run_scenario_log_truncation(nodes, raft_addrs, ok);
        },
        suite_ok,
        raft_addrs);
    run_scenario_real_lock_table(suite_ok, raft_addrs);

    log_line(std::string("[RAFT-TEST] SUMMARY: ") + (suite_ok ? "ALL PASS" : "FAIL"));
    std::cout << "[RAFT-TEST] exit " << (suite_ok ? 0 : 1) << std::endl;
    return suite_ok ? 0 : 1;
}
