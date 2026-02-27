#include "lock_service_impl.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
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

std::string getenv_or_default(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value != nullptr ? value : fallback;
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
    lock_table_.acquire(agent_id, embedding, theta_);
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

    const bool qdrant_ok = upsert_embedding_to_qdrant(agent_id, embedding);
    if (!qdrant_ok) {
        response->set_granted(false);
        response->set_message("qdrant write failed");
        return grpc::Status::OK;
    }

    release_once();
    release_guard.dismiss();

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
    const std::string& agent_id,
    const std::vector<float>& embedding) const {
    if (embedding.empty()) {
        return false;
    }

    if (!ensure_qdrant_collection(embedding.size())) {
        return false;
    }

    std::ostringstream body;
    body << "{\"points\":[{\"id\":\"" << escape_json(agent_id) << "\",\"vector\":[";
    for (size_t i = 0; i < embedding.size(); ++i) {
        if (i > 0) {
            body << ",";
        }
        body << std::setprecision(8) << embedding[i];
    }
    body << "]}]}";

    int status_code = 0;
    std::string response_body;
    const std::string target = "/collections/" + qdrant_collection_ + "/points?wait=true";
    if (!send_http_json("PUT", target, body.str(), status_code, response_body)) {
        std::cout << "[QDRANT] request failed for agent_id=" << agent_id << std::endl;
        return false;
    }

    if (status_code != 200 && status_code != 201) {
        std::cout << "[QDRANT] upsert failed for agent_id=" << agent_id
                  << " status=" << status_code << std::endl;
        return false;
    }
    return true;
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
                  << std::endl;
        return false;
    }

    // 409 is acceptable when the collection already exists.
    return status_code == 200 || status_code == 201 || status_code == 409;
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
