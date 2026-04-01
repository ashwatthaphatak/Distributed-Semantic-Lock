// Implements the thin gRPC wrapper around the RaftNode core.

#include "raft_service_impl.h"

RaftServiceImpl::RaftServiceImpl(RaftNode* raft) : raft_(raft) {}

grpc::Status RaftServiceImpl::RequestVote(grpc::ServerContext*,
                                          const dscc_raft::VoteRequest* request,
                                          dscc_raft::VoteResponse* response) {
    *response = raft_->HandleRequestVote(*request);
    return grpc::Status::OK;
}

grpc::Status RaftServiceImpl::AppendEntries(grpc::ServerContext*,
                                            const dscc_raft::AppendRequest* request,
                                            dscc_raft::AppendResponse* response) {
    *response = raft_->HandleAppendEntries(*request);
    return grpc::Status::OK;
}

grpc::Status RaftServiceImpl::InstallSnapshot(grpc::ServerContext*,
                                              const dscc_raft::SnapshotRequest* request,
                                              dscc_raft::SnapshotResponse* response) {
    *response = raft_->HandleInstallSnapshot(*request);
    return grpc::Status::OK;
}

grpc::Status RaftServiceImpl::GetLeader(grpc::ServerContext*,
                                        const dscc_raft::LeaderQuery*,
                                        dscc_raft::LeaderInfo* response) {
    *response = raft_->HandleGetLeader();
    return grpc::Status::OK;
}
