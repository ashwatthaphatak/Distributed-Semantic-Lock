// Implements the in-memory Raft node used by dscc-node.
// This covers leader election, log replication, and commit/application.

#include "raft_node.h"

#include "threadsafe_log.h"

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>

namespace {

std::string raft_state_name(RaftState state) {
    switch (state) {
        case RaftState::FOLLOWER:
            return "FOLLOWER";
        case RaftState::CANDIDATE:
            return "CANDIDATE";
        case RaftState::LEADER:
            return "LEADER";
    }
    return "UNKNOWN";
}

}  // namespace

RaftNode::RaftNode(std::string node_id,
                   std::string service_address,
                   std::vector<std::string> peer_addresses,
                   ApplyCallback on_commit,
                   RaftConfig config)
    : node_id_(std::move(node_id)),
      service_address_(std::move(service_address)),
      peer_addresses_(std::move(peer_addresses)),
      on_commit_(std::move(on_commit)),
      config_(config) {
    log_.emplace_back();
    log_.back().set_term(0);

    for (const auto& peer_address : peer_addresses_) {
        PeerConnection peer;
        peer.channel = grpc::CreateChannel(peer_address, grpc::InsecureChannelCredentials());
        peers_.emplace(peer_address, std::move(peer));
    }
}

RaftNode::~RaftNode() {
    Stop();
}

void RaftNode::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        ResetElectionDeadlineLocked();
        current_leader_id_.clear();
        current_leader_address_.clear();
        if (peer_addresses_.empty()) {
            current_term_ = std::max<int64_t>(1, current_term_);
            voted_for_ = node_id_;
            BecomeLeaderLocked();
        }
    }

    apply_thread_ = std::thread(&RaftNode::ApplyLoop, this);
    election_timer_thread_ = std::thread(&RaftNode::ElectionTimerLoop, this);
    heartbeat_thread_ = std::thread(&RaftNode::HeartbeatLoop, this);
}

void RaftNode::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    commit_cv_.notify_all();
    apply_cv_.notify_all();

    if (election_timer_thread_.joinable()) {
        election_timer_thread_.join();
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    if (apply_thread_.joinable()) {
        apply_thread_.join();
    }
}

bool RaftNode::Propose(const dscc_raft::LogEntry& entry,
                       std::chrono::milliseconds timeout,
                       int64_t* committed_index) {
    int64_t proposed_index = 0;
    {
        std::unique_lock<std::mutex> lock(mu_);
        if (!running_ || state_ != RaftState::LEADER) {
            return false;
        }

        dscc_raft::LogEntry proposed = entry;
        proposed.set_term(current_term_);
        log_.push_back(proposed);
        proposed_index = static_cast<int64_t>(log_.size()) - 1;

        if (peer_addresses_.empty()) {
            commit_index_ = proposed_index;
            commit_cv_.notify_all();
            apply_cv_.notify_all();
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (running_ && std::chrono::steady_clock::now() < deadline) {
        std::vector<std::thread> replication_threads;
        replication_threads.reserve(peer_addresses_.size());
        for (const auto& peer_address : peer_addresses_) {
            replication_threads.emplace_back([this, peer_address]() {
                ReplicateToFollower(peer_address);
            });
        }
        for (auto& thread : replication_threads) {
            thread.join();
        }

        std::unique_lock<std::mutex> lock(mu_);
        if (commit_index_ >= proposed_index) {
            if (committed_index != nullptr) {
                *committed_index = proposed_index;
            }
            return true;
        }
        if (state_ != RaftState::LEADER) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        commit_cv_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(25)));
    }

    std::unique_lock<std::mutex> lock(mu_);
    if (commit_index_ >= proposed_index) {
        if (committed_index != nullptr) {
            *committed_index = proposed_index;
        }
        return true;
    }
    return false;
}

bool RaftNode::WaitUntilApplied(int64_t index, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    return apply_cv_.wait_for(lock, timeout, [&]() {
        return !running_ || last_applied_ >= index;
    });
}

dscc_raft::VoteResponse RaftNode::HandleRequestVote(const dscc_raft::VoteRequest& request) {
    std::lock_guard<std::mutex> lock(mu_);
    dscc_raft::VoteResponse response;

    if (request.term() < current_term_) {
        response.set_term(current_term_);
        response.set_vote_granted(false);
        return response;
    }

    if (request.term() > current_term_) {
        BecomeFollowerLocked(request.term(), "", "");
    }

    const bool can_vote = voted_for_.empty() || voted_for_ == request.candidate_id();
    const bool up_to_date = IsLogUpToDateLocked(request.last_log_index(),
                                                request.last_log_term());
    if (can_vote && up_to_date) {
        voted_for_ = request.candidate_id();
        ResetElectionDeadlineLocked();
        response.set_vote_granted(true);
    } else {
        response.set_vote_granted(false);
    }

    response.set_term(current_term_);
    return response;
}

dscc_raft::AppendResponse RaftNode::HandleAppendEntries(
    const dscc_raft::AppendRequest& request) {
    std::lock_guard<std::mutex> lock(mu_);
    dscc_raft::AppendResponse response;

    if (request.term() < current_term_) {
        response.set_term(current_term_);
        response.set_success(false);
        return response;
    }

    if (request.term() > current_term_ || state_ != RaftState::FOLLOWER) {
        BecomeFollowerLocked(request.term(),
                             request.leader_id(),
                             request.leader_service_address());
    } else {
        current_leader_id_ = request.leader_id();
        current_leader_address_ = request.leader_service_address();
        ResetElectionDeadlineLocked();
    }

    const int64_t prev_index = request.prev_log_index();
    if (prev_index > 0 &&
        (prev_index >= static_cast<int64_t>(log_.size()) ||
         log_[static_cast<size_t>(prev_index)].term() != request.prev_log_term())) {
        response.set_term(current_term_);
        response.set_success(false);
        if (prev_index < static_cast<int64_t>(log_.size())) {
            const int64_t conflict_term =
                log_[static_cast<size_t>(prev_index)].term();
            response.set_conflict_term(conflict_term);
            int64_t conflict_index = prev_index;
            while (conflict_index > 1 &&
                   log_[static_cast<size_t>(conflict_index - 1)].term() == conflict_term) {
                --conflict_index;
            }
            response.set_conflict_index(conflict_index);
        } else {
            response.set_conflict_index(static_cast<int64_t>(log_.size()));
        }
        return response;
    }

    for (int i = 0; i < request.entries_size(); ++i) {
        const int64_t index = prev_index + 1 + i;
        const dscc_raft::LogEntry& incoming = request.entries(i);
        if (index < static_cast<int64_t>(log_.size())) {
            if (log_[static_cast<size_t>(index)].term() != incoming.term()) {
                log_.resize(static_cast<size_t>(index));
            } else {
                continue;
            }
        }
        log_.push_back(incoming);
    }

    if (request.leader_commit() > commit_index_) {
        commit_index_ =
            std::min<int64_t>(request.leader_commit(),
                              static_cast<int64_t>(log_.size()) - 1);
        commit_cv_.notify_all();
        apply_cv_.notify_all();
    }

    response.set_term(current_term_);
    response.set_success(true);
    response.set_match_index(static_cast<int64_t>(log_.size()) - 1);
    return response;
}

dscc_raft::SnapshotResponse RaftNode::HandleInstallSnapshot(
    const dscc_raft::SnapshotRequest& request) {
    std::lock_guard<std::mutex> lock(mu_);
    dscc_raft::SnapshotResponse response;
    if (request.term() > current_term_) {
        BecomeFollowerLocked(request.term(), request.leader_id(), current_leader_address_);
    }
    response.set_term(current_term_);
    return response;
}

dscc_raft::LeaderInfo RaftNode::HandleGetLeader() const {
    std::lock_guard<std::mutex> lock(mu_);
    dscc_raft::LeaderInfo response;
    response.set_leader_id(state_ == RaftState::LEADER ? node_id_ : current_leader_id_);
    response.set_leader_address(state_ == RaftState::LEADER
                                    ? service_address_
                                    : current_leader_address_);
    response.set_current_term(current_term_);
    response.set_is_leader(state_ == RaftState::LEADER);
    return response;
}

bool RaftNode::IsLeader() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_ == RaftState::LEADER;
}

RaftState RaftNode::State() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

std::string RaftNode::LeaderAddress() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_ == RaftState::LEADER ? service_address_ : current_leader_address_;
}

std::string RaftNode::NodeId() const {
    return node_id_;
}

std::string RaftNode::ServiceAddress() const {
    return service_address_;
}

int64_t RaftNode::CurrentTerm() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_term_;
}

int64_t RaftNode::CommitIndex() const {
    std::lock_guard<std::mutex> lock(mu_);
    return commit_index_;
}

int64_t RaftNode::LastApplied() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_applied_;
}

int64_t RaftNode::LogSize() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int64_t>(log_.size()) - 1;
}

bool RaftNode::Running() const {
    return running_.load();
}

void RaftNode::ElectionTimerLoop() {
    while (running_) {
        bool should_start_election = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (state_ != RaftState::LEADER &&
                std::chrono::steady_clock::now() >= election_deadline_) {
                should_start_election = true;
            }
        }

        if (should_start_election) {
            StartElection();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void RaftNode::HeartbeatLoop() {
    while (running_) {
        std::vector<std::string> peers_to_ping;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (state_ == RaftState::LEADER) {
                peers_to_ping = peer_addresses_;
            }
        }

        std::vector<std::thread> replication_threads;
        replication_threads.reserve(peers_to_ping.size());
        for (const auto& peer_address : peers_to_ping) {
            replication_threads.emplace_back([this, peer_address]() {
                ReplicateToFollower(peer_address);
            });
        }
        for (auto& thread : replication_threads) {
            thread.join();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_ms));
    }
}

void RaftNode::ApplyLoop() {
    while (running_) {
        dscc_raft::LogEntry entry;
        {
            std::unique_lock<std::mutex> lock(mu_);
            apply_cv_.wait(lock, [&]() {
                return !running_ || last_applied_ < commit_index_;
            });
            if (!running_) {
                return;
            }

            ++last_applied_;
            entry = log_[static_cast<size_t>(last_applied_)];
        }

        on_commit_(entry);

        {
            std::lock_guard<std::mutex> lock(mu_);
            apply_cv_.notify_all();
        }
    }
}

void RaftNode::StartElection() {
    int64_t election_term = 0;
    int64_t last_log_index = 0;
    int64_t last_log_term = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_ || state_ == RaftState::LEADER) {
            return;
        }

        state_ = RaftState::CANDIDATE;
        ++current_term_;
        voted_for_ = node_id_;
        current_leader_id_.clear();
        current_leader_address_.clear();
        ResetElectionDeadlineLocked();

        election_term = current_term_;
        last_log_index = LastLogIndexLocked();
        last_log_term = LastLogTermLocked();
    }

    {
        std::ostringstream oss;
        oss << "[RAFT " << node_id_ << "] starting election term=" << election_term;
        log_line(oss.str());
    }

    dscc_raft::VoteRequest request;
    request.set_term(election_term);
    request.set_candidate_id(node_id_);
    request.set_last_log_index(last_log_index);
    request.set_last_log_term(last_log_term);

    int votes = 1;
    for (const auto& peer_address : peer_addresses_) {
        dscc_raft::VoteResponse response;
        if (!SendRequestVote(peer_address, request, response)) {
            continue;
        }

        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) {
            return;
        }
        if (response.term() > current_term_) {
            BecomeFollowerLocked(response.term(), "", "");
            return;
        }
        if (state_ != RaftState::CANDIDATE || current_term_ != election_term) {
            return;
        }
        if (response.vote_granted()) {
            ++votes;
            if (votes >= QuorumSize()) {
                BecomeLeaderLocked();
                break;
            }
        }
    }
}

void RaftNode::BecomeLeaderLocked() {
    state_ = RaftState::LEADER;
    current_leader_id_ = node_id_;
    current_leader_address_ = service_address_;
    next_index_.clear();
    match_index_.clear();
    for (const auto& peer_address : peer_addresses_) {
        next_index_[peer_address] = static_cast<int64_t>(log_.size());
        match_index_[peer_address] = 0;
    }

    std::ostringstream oss;
    oss << "[RAFT " << node_id_ << "] became leader term=" << current_term_;
    log_line(oss.str());
}

void RaftNode::BecomeFollowerLocked(int64_t term,
                                    const std::string& leader_id,
                                    const std::string& leader_address) {
    state_ = RaftState::FOLLOWER;
    current_term_ = std::max(current_term_, term);
    voted_for_.clear();
    current_leader_id_ = leader_id;
    current_leader_address_ = leader_address;
    ResetElectionDeadlineLocked();
}

void RaftNode::ResetElectionDeadlineLocked() {
    election_deadline_ = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(RandomElectionTimeoutMs());
}

void RaftNode::AdvanceCommitIndexLocked() {
    if (state_ != RaftState::LEADER) {
        return;
    }

    for (int64_t index = static_cast<int64_t>(log_.size()) - 1;
         index > commit_index_;
         --index) {
        if (log_[static_cast<size_t>(index)].term() != current_term_) {
            continue;
        }

        int replicated = 1;
        for (const auto& [peer_address, match_index] : match_index_) {
            (void)peer_address;
            if (match_index >= index) {
                ++replicated;
            }
        }

        if (replicated >= QuorumSize()) {
            commit_index_ = index;
            commit_cv_.notify_all();
            apply_cv_.notify_all();
            break;
        }
    }
}

bool RaftNode::IsLogUpToDateLocked(int64_t candidate_last_log_index,
                                   int64_t candidate_last_log_term) const {
    const int64_t local_last_term = LastLogTermLocked();
    if (candidate_last_log_term != local_last_term) {
        return candidate_last_log_term > local_last_term;
    }
    return candidate_last_log_index >= LastLogIndexLocked();
}

bool RaftNode::ReplicateToFollower(const std::string& peer_address) {
    while (running_) {
        dscc_raft::AppendRequest request;
        int64_t sent_term = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (state_ != RaftState::LEADER) {
                return false;
            }

            int64_t next_index = next_index_[peer_address];
            next_index = std::max<int64_t>(1, next_index);
            next_index_[peer_address] = next_index;

            request.set_term(current_term_);
            request.set_leader_id(node_id_);
            request.set_leader_service_address(service_address_);
            request.set_prev_log_index(next_index - 1);
            request.set_prev_log_term(
                log_[static_cast<size_t>(next_index - 1)].term());
            request.set_leader_commit(commit_index_);
            for (size_t i = static_cast<size_t>(next_index); i < log_.size(); ++i) {
                *request.add_entries() = log_[i];
            }
            sent_term = current_term_;
        }

        dscc_raft::AppendResponse response;
        grpc::Status status;
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::milliseconds(config_.rpc_timeout_ms));
            auto stub = dscc_raft::RaftService::NewStub(peers_.at(peer_address).channel);
            status = stub->AppendEntries(&context, request, &response);
        }

        if (!status.ok()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) {
            return false;
        }
        if (response.term() > current_term_) {
            BecomeFollowerLocked(response.term(), "", "");
            return false;
        }
        if (state_ != RaftState::LEADER || current_term_ != sent_term) {
            return false;
        }

        if (response.success()) {
            match_index_[peer_address] =
                std::max(match_index_[peer_address], response.match_index());
            next_index_[peer_address] = match_index_[peer_address] + 1;
            AdvanceCommitIndexLocked();
            return true;
        }

        int64_t retry_index = next_index_[peer_address] - 1;
        if (response.conflict_index() > 0) {
            retry_index = response.conflict_index();
        }
        next_index_[peer_address] = std::max<int64_t>(1, retry_index);
    }

    return false;
}

bool RaftNode::SendRequestVote(const std::string& peer_address,
                               const dscc_raft::VoteRequest& request,
                               dscc_raft::VoteResponse& response) {
    grpc::Status status;
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::milliseconds(config_.rpc_timeout_ms));
        auto stub = dscc_raft::RaftService::NewStub(peers_.at(peer_address).channel);
        status = stub->RequestVote(&context, request, &response);
    }
    return status.ok();
}

int64_t RaftNode::RandomElectionTimeoutMs() const {
    const int min_ms = std::min(config_.election_timeout_min_ms,
                                config_.election_timeout_max_ms);
    const int max_ms = std::max(config_.election_timeout_min_ms,
                                config_.election_timeout_max_ms);
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(min_ms, max_ms);
    return dist(rng);
}

int64_t RaftNode::LastLogIndexLocked() const {
    return static_cast<int64_t>(log_.size()) - 1;
}

int64_t RaftNode::LastLogTermLocked() const {
    return log_.empty() ? 0 : log_.back().term();
}

int RaftNode::QuorumSize() const {
    return static_cast<int>((peer_addresses_.size() + 1) / 2) + 1;
}
