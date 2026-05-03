// Author: Ayush Gala
// Declares the gRPC wrapper for Raft inter-node RPCs and leader discovery.

#pragma once

#include "dscc_raft.grpc.pb.h"
#include "raft_node.h"

class RaftServiceImpl final : public dscc_raft::RaftService::Service {
public:
    explicit RaftServiceImpl(RaftNode* raft);

    // Delegates an inbound vote request from a candidate to the RaftNode election handler.
    grpc::Status RequestVote(grpc::ServerContext* context,
                             const dscc_raft::VoteRequest* request,
                             dscc_raft::VoteResponse* response) override;

    // Delivers a heartbeat or log-replication batch from the leader to the local RaftNode.
    grpc::Status AppendEntries(grpc::ServerContext* context,
                               const dscc_raft::AppendRequest* request,
                               dscc_raft::AppendResponse* response) override;

    // Streams a full snapshot to a lagging follower so it can rejoin the cluster.
    grpc::Status InstallSnapshot(grpc::ServerContext* context,
                                 const dscc_raft::SnapshotRequest* request,
                                 dscc_raft::SnapshotResponse* response) override;

    // Exposes the current leader identity so proxies and clients can redirect writes.
    grpc::Status GetLeader(grpc::ServerContext* context,
                           const dscc_raft::LeaderQuery* request,
                           dscc_raft::LeaderInfo* response) override;

private:
    RaftNode* raft_;
};
