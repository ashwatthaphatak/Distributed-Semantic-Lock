#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>   // add
#include "lock_service_impl.h"
#include <iostream>
#include <cstdlib>

int main() {
    const char* port = std::getenv("PORT");
    std::string server_address = "0.0.0.0:" + std::string(port ? port : "50051");

    LockServiceImpl service;

    grpc::EnableDefaultHealthCheckService(true);                 // optional but useful
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();  // reflection

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    server->Wait();
    return 0;
}