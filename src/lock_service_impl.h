#pragma once

#include <grpcpp/grpcpp.h>
#include "dscc.grpc.pb.h"

class LockServiceImpl final : public dscc::LockService::Service {
public:
    grpc::Status Ping(grpc::ServerContext* context,
                      const dscc::PingRequest* request,
                      dscc::PingResponse* response) override;

    grpc::Status AcquireGuard(grpc::ServerContext* context,
                              const dscc::AcquireRequest* request,
                              dscc::AcquireResponse* response) override;

    grpc::Status ReleaseGuard(grpc::ServerContext* context,
                              const dscc::ReleaseRequest* request,
                              dscc::ReleaseResponse* response) override;
};