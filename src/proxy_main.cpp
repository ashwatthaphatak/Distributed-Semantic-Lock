// Starts the leader-aware dscc proxy that fronts the Raft cluster.

#include "proxy_service_impl.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string getenv_or_default(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value != nullptr ? value : fallback;
}

int read_int_from_env(const char* key, int fallback, int min_value, int max_value) {
    const char* value = std::getenv(key);
    if (value == nullptr) {
        return fallback;
    }

    char* endptr = nullptr;
    const long parsed = std::strtol(value, &endptr, 10);
    if (endptr == value || parsed < min_value || parsed > max_value) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

std::vector<std::string> split_csv(const std::string& input) {
    std::vector<std::string> parts;
    std::stringstream stream(input);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            item.erase(std::remove_if(item.begin(), item.end(), ::isspace), item.end());
            if (!item.empty()) {
                parts.push_back(item);
            }
        }
    }
    return parts;
}

}  // namespace

int main() {
    const std::string port = getenv_or_default("PROXY_PORT", "50050");
    const std::vector<std::string> backend_nodes =
        split_csv(getenv_or_default("BACKEND_NODES", ""));
    const int leader_poll_ms = read_int_from_env("LEADER_POLL_MS", 100, 20, 60000);
    const int request_timeout_ms =
        read_int_from_env("REQUEST_TIMEOUT_MS", 35000, 50, 600000);
    const int leader_rpc_timeout_ms =
        read_int_from_env("LEADER_RPC_TIMEOUT_MS", 750, 50, 60000);

    if (backend_nodes.empty()) {
        std::cerr << "BACKEND_NODES is required for dscc-proxy" << std::endl;
        return 1;
    }

    ProxyServiceImpl service(backend_nodes,
                             leader_poll_ms,
                             request_timeout_ms,
                             leader_rpc_timeout_ms);
    service.Start();

    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder;
    const std::string listen_address = "0.0.0.0:" + port;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (server == nullptr) {
        std::cerr << "Failed to start dscc-proxy" << std::endl;
        service.Stop();
        return 1;
    }

    std::cout << "Proxy listening on " << listen_address << std::endl;
    server->Wait();
    service.Stop();
    return 0;
}
