// Starts the distributed dscc-node runtime.
// This wires the agent-facing lock service and the Raft service onto the node.

#include "active_lock_table.h"
#include "lock_service_impl.h"
#include "raft_node.h"
#include "raft_service_impl.h"

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

float read_float_from_env(const char* key, float fallback, float min_value, float max_value) {
    const char* value = std::getenv(key);
    if (value == nullptr) {
        return fallback;
    }

    char* endptr = nullptr;
    const float parsed = std::strtof(value, &endptr);
    if (endptr == value || parsed < min_value || parsed > max_value) {
        return fallback;
    }
    return parsed;
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
    const std::string node_id = getenv_or_default("NODE_ID", "node-1");
    const std::string port = getenv_or_default("PORT", "50051");
    const std::string raft_port = getenv_or_default("RAFT_PORT", "50061");
    const std::string advertise_host = getenv_or_default("ADVERTISE_HOST", "127.0.0.1");
    const std::string peers_env = getenv_or_default("PEERS", "");

    const float theta = read_float_from_env("THETA", 0.85f, 0.0f, 1.0f);
    const int lock_hold_ms = read_int_from_env("LOCK_HOLD_MS", 0, 0, 600000);
    const int propose_timeout_ms =
        read_int_from_env("RAFT_PROPOSE_TIMEOUT_MS", 5000, 50, 600000);

    RaftConfig raft_config;
    raft_config.heartbeat_ms = read_int_from_env("HEARTBEAT_MS", 75, 10, 5000);
    raft_config.election_timeout_min_ms =
        read_int_from_env("ELECTION_TIMEOUT_MIN_MS", 600, 20, 30000);
    raft_config.election_timeout_max_ms =
        read_int_from_env("ELECTION_TIMEOUT_MAX_MS", 1000, 20, 30000);
    raft_config.rpc_timeout_ms = read_int_from_env("RAFT_RPC_TIMEOUT_MS", 150, 20, 30000);

    const std::string qdrant_host = getenv_or_default("QDRANT_HOST", "qdrant");
    const std::string qdrant_port = getenv_or_default("QDRANT_PORT", "6333");
    const std::string qdrant_collection =
        getenv_or_default("QDRANT_COLLECTION", "dscc_memory");

    const std::string service_address = "0.0.0.0:" + port;
    const std::string raft_address = "0.0.0.0:" + raft_port;
    const std::string advertised_service_address = advertise_host + ":" + port;
    const std::vector<std::string> peer_addresses = split_csv(peers_env);

    ActiveLockTable lock_table;
    RaftNode raft(node_id,
                  advertised_service_address,
                  peer_addresses,
                  [&lock_table](const dscc_raft::LogEntry& entry) {
                      if (entry.op_type() == dscc_raft::LogEntry::ACQUIRE) {
                          std::vector<float> embedding(entry.embedding().begin(),
                                                       entry.embedding().end());
                          lock_table.apply_acquire(entry.agent_id(),
                                                   embedding,
                                                   entry.theta());
                      } else {
                          lock_table.apply_release(entry.agent_id());
                      }
                  },
                  raft_config);
    raft.Start();

    LockServiceImpl lock_service(&raft,
                                 &lock_table,
                                 node_id,
                                 theta,
                                 lock_hold_ms,
                                 propose_timeout_ms,
                                 qdrant_host,
                                 qdrant_port,
                                 qdrant_collection);
    RaftServiceImpl raft_service(&raft);

    grpc::EnableDefaultHealthCheckService(true);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(service_address, grpc::InsecureServerCredentials());
    builder.AddListeningPort(raft_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&lock_service);
    builder.RegisterService(&raft_service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (server == nullptr) {
        std::cerr << "Failed to start dscc-node gRPC server" << std::endl;
        raft.Stop();
        return 1;
    }

    std::cout << "Node " << node_id << " listening on " << service_address
              << " and " << raft_address
              << " advertise=" << advertised_service_address << std::endl;

    server->Wait();
    raft.Stop();
    return 0;
}
