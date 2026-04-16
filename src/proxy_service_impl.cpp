// Implements the leader-aware proxy that fronts the dscc-node cluster.

#include "proxy_service_impl.h"

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>

#include <thread>

ProxyServiceImpl::ProxyServiceImpl(std::vector<std::string> backend_nodes,
                                   int leader_poll_ms,
                                   int request_timeout_ms,
                                   int leader_rpc_timeout_ms)
    : backend_nodes_(std::move(backend_nodes)),
      leader_poll_ms_(leader_poll_ms),
      request_timeout_ms_(request_timeout_ms),
      leader_rpc_timeout_ms_(leader_rpc_timeout_ms) {}

ProxyServiceImpl::~ProxyServiceImpl() {
    Stop();
}

void ProxyServiceImpl::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    poll_thread_ = std::thread(&ProxyServiceImpl::LeaderPollLoop, this);
}

void ProxyServiceImpl::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
}

grpc::Status ProxyServiceImpl::Ping(grpc::ServerContext*,
                                    const dscc::PingRequest* request,
                                    dscc::PingResponse* response) {
    for (const auto& backend : backend_nodes_) {
        grpc::ClientContext client_context;
        client_context.set_deadline(std::chrono::system_clock::now() +
                                    std::chrono::milliseconds(leader_rpc_timeout_ms_));
        auto stub = LockStubFor(backend);
        const grpc::Status status = stub->Ping(&client_context, *request, response);
        if (status.ok()) {
            return status;
        }
    }
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No backend nodes responded to Ping");
}

grpc::Status ProxyServiceImpl::AcquireGuard(grpc::ServerContext*,
                                            const dscc::AcquireRequest* request,
                                            dscc::AcquireResponse* response) {
    return ForwardWithLeaderRetry(
        *request,
        response,
        [](dscc::LockService::Stub* stub,
           grpc::ClientContext* context,
           const dscc::AcquireRequest& forwarded_request,
           dscc::AcquireResponse* forwarded_response) {
            return stub->AcquireGuard(context, forwarded_request, forwarded_response);
        });
}

grpc::Status ProxyServiceImpl::ReleaseGuard(grpc::ServerContext*,
                                            const dscc::ReleaseRequest* request,
                                            dscc::ReleaseResponse* response) {
    return ForwardWithLeaderRetry(
        *request,
        response,
        [](dscc::LockService::Stub* stub,
           grpc::ClientContext* context,
           const dscc::ReleaseRequest& forwarded_request,
           dscc::ReleaseResponse* forwarded_response) {
            return stub->ReleaseGuard(context, forwarded_request, forwarded_response);
        });
}

void ProxyServiceImpl::LeaderPollLoop() {
    while (running_) {
        RefreshLeader();
        std::this_thread::sleep_for(std::chrono::milliseconds(leader_poll_ms_));
    }
}

std::string ProxyServiceImpl::CurrentLeader() {
    std::lock_guard<std::mutex> lock(mu_);
    return current_leader_;
}

void ProxyServiceImpl::SetCurrentLeader(const std::string& leader) {
    std::lock_guard<std::mutex> lock(mu_);
    current_leader_ = leader;
}

std::string ProxyServiceImpl::RefreshLeader() {
    for (const auto& backend : backend_nodes_) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::milliseconds(leader_rpc_timeout_ms_));
        auto stub = RaftStubFor(backend);
        dscc_raft::LeaderQuery request;
        dscc_raft::LeaderInfo response;
        const grpc::Status status = stub->GetLeader(&context, request, &response);
        if (!status.ok()) {
            continue;
        }

        if (response.is_leader() && !response.leader_address().empty()) {
            SetCurrentLeader(response.leader_address());
            return response.leader_address();
        }
        if (!response.leader_address().empty()) {
            SetCurrentLeader(response.leader_address());
            return response.leader_address();
        }
    }

    SetCurrentLeader("");
    return "";
}

std::string ProxyServiceImpl::ExtractLeaderRedirect(
    const grpc::ClientContext& context) const {
    const auto& metadata = context.GetServerTrailingMetadata();
    const auto it = metadata.find("leader-address");
    if (it == metadata.end()) {
        return "";
    }
    return std::string(it->second.data(), it->second.length());
}

std::shared_ptr<grpc::Channel> ProxyServiceImpl::ChannelFor(const std::string& target) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = channels_.find(target);
    if (it != channels_.end()) {
        return it->second;
    }

    std::shared_ptr<grpc::Channel> channel =
        grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    channels_.emplace(target, channel);
    return channel;
}

std::unique_ptr<dscc::LockService::Stub> ProxyServiceImpl::LockStubFor(
    const std::string& target) {
    return dscc::LockService::NewStub(ChannelFor(target));
}

std::unique_ptr<dscc_raft::RaftService::Stub> ProxyServiceImpl::RaftStubFor(
    const std::string& target) {
    return dscc_raft::RaftService::NewStub(ChannelFor(target));
}

template <typename Request, typename Response, typename RpcFn>
grpc::Status ProxyServiceImpl::ForwardWithLeaderRetry(const Request& request,
                                                      Response* response,
                                                      RpcFn rpc_fn) {
    std::string target = RefreshLeader();
    if (target.empty()) {
        target = CurrentLeader();
    }
    if (target.empty()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No leader available");
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        // Retry only on leader churn and transport-level failures. All other
        // semantic/service errors should surface directly to the caller.
        grpc::ClientContext client_context;
        client_context.set_deadline(std::chrono::system_clock::now() +
                                    std::chrono::milliseconds(request_timeout_ms_));
        auto stub = LockStubFor(target);
        grpc::Status status = rpc_fn(stub.get(), &client_context, request, response);
        if (status.ok()) {
            SetCurrentLeader(target);
            return status;
        }

        const std::string redirected = ExtractLeaderRedirect(client_context);
        if (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION &&
            status.error_message() == "NOT_LEADER") {
            target = redirected.empty() ? RefreshLeader() : redirected;
            if (!target.empty()) {
                SetCurrentLeader(target);
                continue;
            }
        }

        if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
            status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
            target = RefreshLeader();
            if (!target.empty()) {
                continue;
            }
        }

        return status;
    }

    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "Leader changed during request forwarding");
}
