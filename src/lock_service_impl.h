#pragma once

#include <grpcpp/grpcpp.h>
#include "dscc.grpc.pb.h"
#include "active_lock_table.h"

#include <cstddef>
#include <string>
#include <vector>

class LockServiceImpl final : public dscc::LockService::Service {
public:
    LockServiceImpl();

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
    bool upsert_embedding_to_qdrant(const std::string& agent_id,
                                    const std::vector<float>& embedding) const;

    bool ensure_qdrant_collection(size_t vector_size) const;

    bool send_http_json(const std::string& method,
                        const std::string& target,
                        const std::string& body,
                        int& status_code,
                        std::string& response_body) const;

    ActiveLockTable lock_table_;
    float theta_;
    std::string qdrant_host_;
    std::string qdrant_port_;
    std::string qdrant_collection_;
};
