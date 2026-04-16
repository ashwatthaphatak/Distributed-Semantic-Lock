// Runs in-process Raft tests over real localhost gRPC servers.
// This validates election, replication, and follower catch-up before lock integration.

#include "raft_node.h"
#include "raft_service_impl.h"
#include "threadsafe_log.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

std::vector<std::string> all_raft_addresses() {
    return {"127.0.0.1:56061", "127.0.0.1:56062", "127.0.0.1:56063"};
}

RaftConfig test_config() {
    RaftConfig config;
    config.heartbeat_ms = 40;
    config.election_timeout_min_ms = 140;
    config.election_timeout_max_ms = 240;
    config.rpc_timeout_ms = 150;
    return config;
}

std::unique_ptr<TestNode> make_node(size_t index) {
    const std::vector<std::string> raft_addresses = all_raft_addresses();
    auto node = std::make_unique<TestNode>();
    node->node_id = "node-" + std::to_string(index + 1);
    node->service_address = "127.0.0.1:" + std::to_string(56051 + static_cast<int>(index));
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
    node->raft->Start();
    return node;
}

void stop_node(TestNode& node) {
    node.raft->Stop();
    if (node.server != nullptr) {
        node.server->Shutdown();
    }
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
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

size_t applied_count(const TestNode& node) {
    std::lock_guard<std::mutex> lock(node.applied.mu);
    return node.applied.entries.size();
}

bool wait_for_applied_count(const TestNode& node,
                            size_t expected,
                            std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (applied_count(node) >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

bool expect_true(const std::string& label, bool condition, bool& overall_pass) {
    log_line(label + ": " + (condition ? "PASS" : "FAIL"));
    if (!condition) {
        overall_pass = false;
    }
    return condition;
}

}  // namespace

int main() {
    bool overall_pass = true;
    {
        std::vector<std::unique_ptr<TestNode>> nodes;
        nodes.push_back(make_node(0));
        nodes.push_back(make_node(1));
        nodes.push_back(make_node(2));

        int first_leader = -1;
        expect_true("Leader elected",
                    wait_for_leader(nodes, &first_leader, std::chrono::seconds(5)),
                    overall_pass);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int stable_leader = -1;
        expect_true("Leader stays stable under heartbeats",
                    wait_for_leader(nodes, &stable_leader, std::chrono::seconds(2)) &&
                        stable_leader == first_leader,
                    overall_pass);

        stop_node(*nodes[static_cast<size_t>(first_leader)]);
        nodes[static_cast<size_t>(first_leader)].reset();

        int replacement_leader = -1;
        expect_true("New leader elected after failure",
                    wait_for_leader(nodes, &replacement_leader, std::chrono::seconds(5)),
                    overall_pass);

        dscc_raft::LogEntry entry;
        entry.set_op_type(dscc_raft::LogEntry::ACQUIRE);
        entry.set_agent_id("raft-test-entry");
        entry.add_embedding(1.0f);
        entry.add_embedding(0.0f);
        entry.set_theta(0.85f);

        int64_t committed_index = 0;
        expect_true("Leader proposal commits to quorum",
                    nodes[static_cast<size_t>(replacement_leader)]->raft->Propose(
                        entry, std::chrono::seconds(2), &committed_index),
                    overall_pass);
        expect_true("Leader apply callback ran",
                    nodes[static_cast<size_t>(replacement_leader)]->raft->WaitUntilApplied(
                        committed_index, std::chrono::seconds(2)),
                    overall_pass);

        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i] != nullptr) {
                expect_true("Committed entry visible on live node " + std::to_string(i + 1),
                            wait_for_applied_count(*nodes[i], 1, std::chrono::seconds(2)),
                            overall_pass);
            }
        }

        for (auto& node : nodes) {
            if (node != nullptr) {
                stop_node(*node);
            }
        }
    }

    {
        std::vector<std::unique_ptr<TestNode>> nodes;
        nodes.push_back(make_node(0));
        nodes.push_back(make_node(1));
        nodes.push_back(make_node(2));

        int leader = -1;
        expect_true("Fresh cluster leader elected",
                    wait_for_leader(nodes, &leader, std::chrono::seconds(5)),
                    overall_pass);

        size_t lagging_index = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (static_cast<int>(i) != leader) {
                lagging_index = i;
                break;
            }
        }

        stop_node(*nodes[lagging_index]);
        nodes[lagging_index].reset();

        for (int i = 0; i < 10; ++i) {
            dscc_raft::LogEntry replicated;
            replicated.set_op_type(dscc_raft::LogEntry::RELEASE);
            replicated.set_agent_id("catch-up-" + std::to_string(i));
            int64_t index = 0;
            const bool committed =
                nodes[static_cast<size_t>(leader)]->raft->Propose(
                    replicated, std::chrono::seconds(2), &index);
            expect_true("Commit while one follower is down " + std::to_string(i),
                        committed,
                        overall_pass);
        }

        nodes[lagging_index] = make_node(lagging_index);
        expect_true("Restarted follower catches up",
                    wait_for_applied_count(*nodes[lagging_index], 10, std::chrono::seconds(6)),
                    overall_pass);

        for (auto& node : nodes) {
            if (node != nullptr) {
                stop_node(*node);
            }
        }
    }

    std::cout << "Final summary: " << (overall_pass ? "PASS" : "FAIL") << std::endl;
    return overall_pass ? 0 : 1;
}
