// Starts the dscc-node gRPC server.
// This is the executable entry point for the semantic lock manager service.
// It wires the LockServiceImpl onto the configured network port.

#include <grpcpp/grpcpp.h>
#include "lock_service_impl.h"
#include <iostream>
#include <cstdlib>

int main() {
    const char* port = std::getenv("PORT");
    std::string server_address = "0.0.0.0:" + std::string(port ? port : "50051");

    LockServiceImpl service;

    grpc::EnableDefaultHealthCheckService(true);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    server->Wait();
    return 0;
}
