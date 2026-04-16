// Declares the in-memory Raft node used to replicate lock-table operations.
// This owns leader election, log replication, and commit/apply sequencing.

#pragma once

#include "dscc_raft.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class RaftState { FOLLOWER, CANDIDATE, LEADER };

using ApplyCallback = std::function<void(const dscc_raft::LogEntry&)>;

struct RaftConfig {
    int heartbeat_ms = 75;
    int election_timeout_min_ms = 600;
    int election_timeout_max_ms = 1000;
    int rpc_timeout_ms = 150;
};

class RaftNode {
public:
    RaftNode(std::string node_id,
             std::string service_address,
             std::vector<std::string> peer_addresses,
             ApplyCallback on_commit,
             RaftConfig config = {});
    ~RaftNode();

    void Start();
    void Stop();

    bool Propose(const dscc_raft::LogEntry& entry,
                 std::chrono::milliseconds timeout,
                 int64_t* committed_index = nullptr);

    bool WaitUntilApplied(int64_t index, std::chrono::milliseconds timeout);

    dscc_raft::VoteResponse HandleRequestVote(const dscc_raft::VoteRequest& request);
    dscc_raft::AppendResponse HandleAppendEntries(const dscc_raft::AppendRequest& request);
    dscc_raft::SnapshotResponse HandleInstallSnapshot(
        const dscc_raft::SnapshotRequest& request);
    dscc_raft::LeaderInfo HandleGetLeader() const;

    bool IsLeader() const;
    RaftState State() const;
    std::string LeaderAddress() const;
    std::string NodeId() const;
    std::string ServiceAddress() const;
    int64_t CurrentTerm() const;
    int64_t CommitIndex() const;
    int64_t LastApplied() const;
    int64_t LogSize() const;
    bool Running() const;

private:
    struct PeerConnection {
        std::shared_ptr<grpc::Channel> channel;
    };

    void ElectionTimerLoop();
    void HeartbeatLoop();
    void ApplyLoop();

    void StartElection();
    void BecomeLeaderLocked();
    void BecomeFollowerLocked(int64_t term,
                              const std::string& leader_id,
                              const std::string& leader_address);
    void ResetElectionDeadlineLocked();
    void AdvanceCommitIndexLocked();
    bool IsLogUpToDateLocked(int64_t candidate_last_log_index,
                             int64_t candidate_last_log_term) const;

    bool ReplicateToFollower(const std::string& peer_address);
    bool SendRequestVote(const std::string& peer_address,
                         const dscc_raft::VoteRequest& request,
                         dscc_raft::VoteResponse& response);

    int64_t RandomElectionTimeoutMs() const;
    int64_t LastLogIndexLocked() const;
    int64_t LastLogTermLocked() const;
    int QuorumSize() const;

    std::string node_id_;
    std::string service_address_;
    std::vector<std::string> peer_addresses_;
    ApplyCallback on_commit_;
    RaftConfig config_;

    mutable std::mutex mu_;
    std::condition_variable commit_cv_;
    std::condition_variable apply_cv_;

    int64_t current_term_ = 0;
    std::string voted_for_;
    std::vector<dscc_raft::LogEntry> log_;

    int64_t commit_index_ = 0;
    int64_t last_applied_ = 0;
    RaftState state_ = RaftState::FOLLOWER;
    std::string current_leader_id_;
    std::string current_leader_address_;
    std::unordered_map<std::string, int64_t> next_index_;
    std::unordered_map<std::string, int64_t> match_index_;
    std::unordered_map<std::string, PeerConnection> peers_;

    std::chrono::steady_clock::time_point election_deadline_;
    std::atomic<bool> running_{false};
    std::thread election_timer_thread_;
    std::thread heartbeat_thread_;
    std::thread apply_thread_;
};
