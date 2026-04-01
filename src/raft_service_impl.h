// Declares the gRPC wrapper for Raft inter-node RPCs and leader discovery.

#pragma once

#include "dscc_raft.grpc.pb.h"
#include "raft_node.h"

class RaftServiceImpl final : public dscc_raft::RaftService::Service {
public:
    explicit RaftServiceImpl(RaftNode* raft);

    grpc::Status RequestVote(grpc::ServerContext* context,
                             const dscc_raft::VoteRequest* request,
                             dscc_raft::VoteResponse* response) override;

    grpc::Status AppendEntries(grpc::ServerContext* context,
                               const dscc_raft::AppendRequest* request,
                               dscc_raft::AppendResponse* response) override;

    grpc::Status InstallSnapshot(grpc::ServerContext* context,
                                 const dscc_raft::SnapshotRequest* request,
                                 dscc_raft::SnapshotResponse* response) override;

    grpc::Status GetLeader(grpc::ServerContext* context,
                           const dscc_raft::LeaderQuery* request,
                           dscc_raft::LeaderInfo* response) override;

private:
    RaftNode* raft_;
};
