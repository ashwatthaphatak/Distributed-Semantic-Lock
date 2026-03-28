// Implements the gRPC lock service used by dscc-node.
// This file bridges the semantic lock table and the Qdrant write path.
// It is the server-side core that the end-to-end bench exercises.

#include "lock_service_impl.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <cstdint>
#include <limits>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

namespace {

class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> fn)
        : fn_(std::move(fn)), active_(true) {}

    ~ScopeExit() {
        if (active_) {
            fn_();
        }
    }

    void dismiss() {
        active_ = false;
    }

private:
    std::function<void()> fn_;
    bool active_;
};

float read_theta_from_env() {
    constexpr float kDefaultTheta = 0.85f;
    const char* theta_env = std::getenv("THETA");
    if (theta_env == nullptr) {
        return kDefaultTheta;
    }

    char* endptr = nullptr;
    const float parsed = std::strtof(theta_env, &endptr);
    if (endptr == theta_env || parsed < 0.0f || parsed > 1.0f) {
        return kDefaultTheta;
    }
    return parsed;
}

int read_lock_hold_ms_from_env() {
    const char* hold_env = std::getenv("LOCK_HOLD_MS");
    if (hold_env == nullptr) {
        return 0;
    }

    char* endptr = nullptr;
    const long parsed = std::strtol(hold_env, &endptr, 10);
    if (endptr == hold_env || parsed < 0L || parsed > 600000L) {
        return 0;
    }
    return static_cast<int>(parsed);
}

std::string getenv_or_default(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value != nullptr ? value : fallback;
}

int64_t make_numeric_point_id(const std::string& agent_id, int64_t timestamp_unix_ms) {
    constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;

    uint64_t hash = kFnvOffset;
    for (unsigned char c : agent_id) {
        hash ^= static_cast<uint64_t>(c);
        hash *= kFnvPrime;
    }

    const uint64_t mixed =
        (static_cast<uint64_t>(timestamp_unix_ms) << 22) ^ (hash & ((1ULL << 22) - 1ULL));
    return static_cast<int64_t>(mixed & static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
}

std::string escape_json(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (const char c : input) {
        switch (c) {
            case '\"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output.push_back(c);
                break;
        }
    }
    return output;
}

bool send_all(int socket_fd, const std::string& payload) {
    size_t total_sent = 0;
    while (total_sent < payload.size()) {
        const ssize_t sent = ::send(socket_fd,
                                    payload.data() + total_sent,
                                    payload.size() - total_sent,
                                    0);
        if (sent <= 0) {
            return false;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

}  // namespace

LockServiceImpl::LockServiceImpl()
    : theta_(read_theta_from_env()),
      lock_hold_ms_(read_lock_hold_ms_from_env()),
      qdrant_host_(getenv_or_default("QDRANT_HOST", "qdrant")),
      qdrant_port_(getenv_or_default("QDRANT_PORT", "6333")),
      qdrant_collection_(getenv_or_default("QDRANT_COLLECTION", "dscc_memory")) {}

grpc::Status LockServiceImpl::Ping(
    grpc::ServerContext*,
    const dscc::PingRequest* request,
    dscc::PingResponse* response) {

    response->set_message("pong to " + request->from_node());
    return grpc::Status::OK;
}

grpc::Status LockServiceImpl::AcquireGuard(
    grpc::ServerContext*,
    const dscc::AcquireRequest* request,
    dscc::AcquireResponse* response) {
    const std::string agent_id = request->agent_id();
    const std::vector<float> embedding(request->embedding().begin(),
                                       request->embedding().end());
    const std::string payload_text = request->payload_text();
    const std::string source_file = request->source_file();
    const auto now_ms = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    const int64_t timestamp_unix_ms =
        request->timestamp_unix_ms() > 0 ? request->timestamp_unix_ms() : now_ms();
    const int64_t point_id = make_numeric_point_id(agent_id, timestamp_unix_ms);
    const int64_t server_received_unix_ms = now_ms();

    if (agent_id.empty()) {
        response->set_granted(false);
        response->set_message("agent_id is required");
        return grpc::Status::OK;
    }
    if (embedding.empty()) {
        response->set_granted(false);
        response->set_message("embedding is required");
        return grpc::Status::OK;
    }

    std::cout << "[TX " << agent_id << "] attempting acquire" << std::endl;
    response->set_server_received_unix_ms(server_received_unix_ms);
    const AcquireTrace acquire_trace = lock_table_.acquire(agent_id, embedding, theta_);
    const int64_t lock_acquired_unix_ms = now_ms();
    response->set_lock_acquired_unix_ms(lock_acquired_unix_ms);
    response->set_lock_wait_ms(lock_acquired_unix_ms - server_received_unix_ms);
    response->set_blocking_similarity_score(acquire_trace.blocking_similarity_score);
    if (!acquire_trace.blocking_agent_id.empty()) {
        response->set_blocking_agent_id(acquire_trace.blocking_agent_id);
    }
    std::cout << "[TX " << agent_id << "] acquired lock (active count = "
              << lock_table_.size() << ")" << std::endl;

    bool released = false;
    auto release_once = [&]() {
        if (!released) {
            lock_table_.release(agent_id);
            released = true;
            std::cout << "[TX " << agent_id << "] released lock (active count = "
                      << lock_table_.size() << ")" << std::endl;
        }
    };
    ScopeExit release_guard(release_once);

    const bool qdrant_ok = upsert_embedding_to_qdrant(point_id,
                                                      agent_id,
                                                      payload_text,
                                                      source_file,
                                                      timestamp_unix_ms,
                                                      embedding);
    if (!qdrant_ok) {
        response->set_granted(false);
        response->set_message("qdrant write failed");
        return grpc::Status::OK;
    }
    response->set_qdrant_write_complete_unix_ms(now_ms());

    if (lock_hold_ms_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(lock_hold_ms_));
    }

    release_once();
    release_guard.dismiss();

    response->set_lock_released_unix_ms(now_ms());
    response->set_granted(true);
    response->set_message("granted and committed");
    return grpc::Status::OK;
}

grpc::Status LockServiceImpl::ReleaseGuard(
    grpc::ServerContext*,
    const dscc::ReleaseRequest* request,
    dscc::ReleaseResponse* response) {
    const std::string agent_id = request->agent_id();
    if (agent_id.empty()) {
        response->set_success(false);
        return grpc::Status::OK;
    }

    lock_table_.release(agent_id);
    std::cout << "[TX " << agent_id << "] released lock (active count = "
              << lock_table_.size() << ")" << std::endl;
    response->set_success(true);
    return grpc::Status::OK;
}

bool LockServiceImpl::upsert_embedding_to_qdrant(
    int64_t point_id,
    const std::string& agent_id,
    const std::string& payload_text,
    const std::string& source_file,
    int64_t timestamp_unix_ms,
    const std::vector<float>& embedding) const {
    if (embedding.empty()) {
        return false;
    }

    if (!ensure_qdrant_collection(embedding.size())) {
        return false;
    }

    std::ostringstream body;
    body << "{\"points\":[{\"id\":" << point_id << ",\"vector\":[";
    for (size_t i = 0; i < embedding.size(); ++i) {
        if (i > 0) {
            body << ",";
        }
        body << std::setprecision(8) << embedding[i];
    }
    body << "],\"payload\":{"
         << "\"agent_id\":\"" << escape_json(agent_id) << "\","
         << "\"source_file\":\"" << escape_json(source_file) << "\","
         << "\"timestamp_unix_ms\":" << timestamp_unix_ms << ","
         << "\"raw_text\":\"" << escape_json(payload_text) << "\""
         << "}}]}";

    const std::string target = "/collections/" + qdrant_collection_ + "/points?wait=true";
    for (int attempt = 1; attempt <= 3; ++attempt) {
        int status_code = 0;
        std::string response_body;
        if (!send_http_json("PUT", target, body.str(), status_code, response_body)) {
            std::cout << "[QDRANT] request failed for agent_id=" << agent_id
                      << " target=" << target
                      << " attempt=" << attempt << std::endl;
            if (attempt < 3) {
                std::this_thread::sleep_for(std::chrono::milliseconds(75 * attempt));
                continue;
            }
            return false;
        }

        if (status_code == 200 || status_code == 201) {
            return true;
        }

        const bool retryable = status_code == 500 &&
            response_body.find("Please retry") != std::string::npos;
        std::cout << "[QDRANT] upsert failed for agent_id=" << agent_id
                  << " status=" << status_code
                  << " attempt=" << attempt
                  << " response=" << response_body << std::endl;
        if (retryable && attempt < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(75 * attempt));
            continue;
        }
        return false;
    }
    return false;
}

bool LockServiceImpl::ensure_qdrant_collection(size_t vector_size) const {
    if (vector_size == 0) {
        return false;
    }

    std::ostringstream body;
    body << "{\"vectors\":{\"size\":" << vector_size
         << ",\"distance\":\"Cosine\"}}";

    int status_code = 0;
    std::string response_body;
    const std::string target = "/collections/" + qdrant_collection_;
    if (!send_http_json("PUT", target, body.str(), status_code, response_body)) {
        std::cout << "[QDRANT] could not ensure collection " << qdrant_collection_
                  << " target=" << target << std::endl;
        return false;
    }

    const std::string normalized = [&response_body]() {
        std::string lowered = response_body;
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lowered;
    }();

    // Qdrant may report an existing collection as 409 or 400 depending on the
    // timing of concurrent create requests after a delete/recreate cycle.
    if (status_code == 200 ||
        status_code == 201 ||
        status_code == 409 ||
        (status_code == 400 &&
         normalized.find("already exists") != std::string::npos)) {
        return true;
    }

    std::cout << "[QDRANT] ensure collection failed collection=" << qdrant_collection_
              << " status=" << status_code
              << " response=" << response_body << std::endl;
    return false;
}

bool LockServiceImpl::send_http_json(const std::string& method,
                                     const std::string& target,
                                     const std::string& body,
                                     int& status_code,
                                     std::string& response_body) const {
    status_code = 0;
    response_body.clear();

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addresses = nullptr;
    const int address_result = ::getaddrinfo(qdrant_host_.c_str(),
                                             qdrant_port_.c_str(),
                                             &hints,
                                             &addresses);
    if (address_result != 0) {
        std::cout << "[QDRANT] DNS resolution failed for host=" << qdrant_host_
                  << " error=" << ::gai_strerror(address_result) << std::endl;
        return false;
    }

    int socket_fd = -1;
    for (struct addrinfo* addr = addresses; addr != nullptr; addr = addr->ai_next) {
        socket_fd = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        if (::connect(socket_fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            break;
        }
        ::close(socket_fd);
        socket_fd = -1;
    }
    ::freeaddrinfo(addresses);

    if (socket_fd < 0) {
        std::cout << "[QDRANT] connect failed host=" << qdrant_host_
                  << " port=" << qdrant_port_
                  << " errno=" << errno
                  << " message=" << std::strerror(errno) << std::endl;
        return false;
    }

    std::ostringstream request;
    request << method << " " << target << " HTTP/1.1\r\n";
    request << "Host: " << qdrant_host_ << ":" << qdrant_port_ << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Connection: close\r\n";
    request << "Content-Length: " << body.size() << "\r\n\r\n";
    request << body;

    const std::string payload = request.str();
    if (!send_all(socket_fd, payload)) {
        std::cout << "[QDRANT] send failed errno=" << errno
                  << " message=" << std::strerror(errno) << std::endl;
        ::close(socket_fd);
        return false;
    }

    constexpr size_t kBufferSize = 4096;
    char buffer[kBufferSize];
    for (;;) {
        const ssize_t read_count = ::recv(socket_fd, buffer, kBufferSize, 0);
        if (read_count < 0) {
            std::cout << "[QDRANT] recv failed errno=" << errno
                      << " message=" << std::strerror(errno) << std::endl;
            ::close(socket_fd);
            return false;
        }
        if (read_count == 0) {
            break;
        }
        response_body.append(buffer, static_cast<size_t>(read_count));
    }
    ::close(socket_fd);

    std::istringstream response_stream(response_body);
    std::string http_version;
    response_stream >> http_version >> status_code;
    return !http_version.empty() && status_code > 0;
}
