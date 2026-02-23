#include "lock_service_impl.h"

grpc::Status LockServiceImpl::Ping(
    grpc::ServerContext*,
    const dscc::PingRequest* request,
    dscc::PingResponse* response) {

    response->set_message("pong to " + request->from_node());
    return grpc::Status::OK;
}

grpc::Status LockServiceImpl::AcquireGuard(
    grpc::ServerContext*,
    const dscc::AcquireRequest*,
    dscc::AcquireResponse* response) {

    response->set_granted(true);
    response->set_message("stub: granted");
    return grpc::Status::OK;
}

grpc::Status LockServiceImpl::ReleaseGuard(
    grpc::ServerContext*,
    const dscc::ReleaseRequest*,
    dscc::ReleaseResponse* response) {

    response->set_success(true);
    return grpc::Status::OK;
}