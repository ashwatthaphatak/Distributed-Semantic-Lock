// Declares the leader-aware front-door proxy for dscc-node.

#pragma once

#include "dscc.grpc.pb.h"
#include "dscc_raft.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class ProxyServiceImpl final : public dscc::LockService::Service {
public:
    ProxyServiceImpl(std::vector<std::string> backend_nodes,
                     int leader_poll_ms,
                     int request_timeout_ms,
                     int leader_rpc_timeout_ms);
    ~ProxyServiceImpl();

    void Start();
    void Stop();

    grpc::Status Ping(grpc::ServerContext* context,
                      const dscc::PingRequest* request,
                      dscc::PingResponse* response) override;

    grpc::Status AcquireGuard(grpc::ServerContext* context,
                              const dscc::AcquireRequest* request,
                              dscc::AcquireResponse* response) override;

    grpc::Status ReleaseGuard(grpc::ServerContext* context,
                              const dscc::ReleaseRequest* request,
                              dscc::ReleaseResponse* response) override;

private:
    void LeaderPollLoop();
    std::string CurrentLeader();
    void SetCurrentLeader(const std::string& leader);
    std::string RefreshLeader();
    std::string ExtractLeaderRedirect(const grpc::ClientContext& context) const;

    std::shared_ptr<grpc::Channel> ChannelFor(const std::string& target);
    std::unique_ptr<dscc::LockService::Stub> LockStubFor(const std::string& target);
    std::unique_ptr<dscc_raft::RaftService::Stub> RaftStubFor(const std::string& target);

    template <typename Request, typename Response, typename RpcFn>
    grpc::Status ForwardWithLeaderRetry(const Request& request,
                                        Response* response,
                                        RpcFn rpc_fn);

    std::vector<std::string> backend_nodes_;
    int leader_poll_ms_;
    int request_timeout_ms_;
    int leader_rpc_timeout_ms_;

    mutable std::mutex mu_;
    // Channels are cached per backend so leader flips do not recreate gRPC
    // connections on every forwarded request.
    std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> channels_;
    std::string current_leader_;

    std::atomic<bool> running_{false};
    std::thread poll_thread_;
};
