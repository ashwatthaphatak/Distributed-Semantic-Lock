// Runs the real end-to-end demo: text files -> embeddings -> DSLM -> Qdrant.
// This is the primary professor-facing test harness for the current repo.
// It coordinates Docker startup, scenario execution, validation, and readable output.

#include "dscc.grpc.pb.h"
#include "threadsafe_log.h"

#include <grpcpp/grpcpp.h>

#include <cctype>
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

namespace ansi {
constexpr const char* kReset = "\033[0m";
constexpr const char* kBlue = "\033[34m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kMagenta = "\033[35m";
constexpr const char* kCyan = "\033[36m";
}  // namespace ansi

struct Config {
    std::string project_root = DSLM_PROJECT_ROOT;
    std::string input_dir = DSLM_PROJECT_ROOT "/demo_inputs";
    std::string embedding_image = "ollama/ollama:latest";
    std::string model_id = "all-minilm:latest";
    std::string collection = "dscc_memory_e2e";
    std::string embedding_host = "127.0.0.1";
    std::string embedding_port = "7997";
    std::string qdrant_host = "127.0.0.1";
    std::string qdrant_port = "6333";
    std::string dscc_target = "127.0.0.1:50051";
    float theta = 0.78f;
    int lock_hold_ms = 750;
    bool teardown_on_exit = false;
};

struct HttpResponse {
    int status_code = 0;
    std::string body;
};

struct ShellCommandResult {
    int exit_code = 0;
    std::string output;
};

struct AgentDocument {
    char label = '?';
    std::string source_file;
    std::string text;
    std::vector<float> embedding;
};

struct AgentOutcome {
    char label = '?';
    std::string agent_id;
    std::string display_name;
    grpc::Status status;
    dscc::AcquireResponse response;
    std::vector<float> embedding;
    int64_t submit_ms = 0;
    int64_t embedding_start_ms = 0;
    int64_t embedding_finish_ms = 0;
    int64_t dslm_enter_ms = 0;
    int64_t dslm_exit_ms = 0;
    int64_t finish_ms = 0;
    int64_t elapsed_ms = 0;
};

struct Scenario {
    std::string name;
    std::vector<char> labels;
    std::vector<std::pair<char, char>> expected_conflicts;
    bool expect_distinct_completion = false;
};

struct TimelineEvent {
    int64_t time_ms = 0;
    size_t sequence = 0;
    const char* color = ansi::kBlue;
    std::string text;
};

void log_tagged(const char* color,
                const std::string& tag,
                const std::string& message) {
    log_line(std::string(color) + "[" + tag + "]" + ansi::kReset + " " + message);
}

void info(const std::string& message) {
    log_tagged(ansi::kBlue, "INFO", message);
}

void success(const std::string& message) {
    log_tagged(ansi::kGreen, "SUCCESS", message);
}

void warn(const std::string& message) {
    log_tagged(ansi::kYellow, "WARN", message);
}

void error(const std::string& message) {
    log_tagged(ansi::kRed, "ERROR", message);
}

void section_header(const std::string& title) {
    log_line("");
    log_line(std::string(ansi::kMagenta) +
             "============================================================" +
             ansi::kReset);
    log_line(std::string(ansi::kMagenta) + title + ansi::kReset);
    log_line(std::string(ansi::kMagenta) +
             "============================================================" +
             ansi::kReset);
}

std::string format_float(float value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << value;
    return oss.str();
}

std::string agent_name(char label) {
    return "Agent " + std::string(1, label);
}

std::string display_agent_id(const std::string& agent_id) {
    const size_t pos = agent_id.rfind("-agent-");
    if (pos != std::string::npos && pos + 7 < agent_id.size()) {
        return agent_name(agent_id[pos + 7]);
    }
    return agent_id;
}

std::string shell_quote(const std::string& input) {
    std::string quoted = "'";
    for (char c : input) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted += "'";
    return quoted;
}

std::string lowercase_copy(std::string text) {
    std::transform(text.begin(),
                   text.end(),
                   text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

std::string escape_json(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (char c : input) {
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

HttpResponse send_http_request(const std::string& host,
                               const std::string& port,
                               const std::string& method,
                               const std::string& target,
                               const std::string& body = {},
                               const std::string& content_type = "application/json") {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addresses = nullptr;
    const int address_result =
        ::getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
    if (address_result != 0) {
        throw std::runtime_error("DNS resolution failed for " + host + ":" + port);
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
        throw std::runtime_error("connect failed for " + host + ":" + port);
    }

    std::ostringstream request;
    request << method << " " << target << " HTTP/1.1\r\n";
    request << "Host: " << host << ":" << port << "\r\n";
    request << "Connection: close\r\n";
    if (!body.empty()) {
        request << "Content-Type: " << content_type << "\r\n";
        request << "Content-Length: " << body.size() << "\r\n";
    }
    request << "\r\n";
    request << body;

    if (!send_all(socket_fd, request.str())) {
        ::close(socket_fd);
        throw std::runtime_error("send failed for " + host + ":" + port);
    }

    std::string raw_response;
    char buffer[4096];
    while (true) {
        const ssize_t received = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received < 0) {
            ::close(socket_fd);
            throw std::runtime_error("recv failed for " + host + ":" + port);
        }
        if (received == 0) {
            break;
        }
        raw_response.append(buffer, static_cast<size_t>(received));
    }
    ::close(socket_fd);

    const size_t header_end = raw_response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        throw std::runtime_error("malformed HTTP response from " + host + ":" + port);
    }

    std::istringstream response_stream(raw_response.substr(0, header_end));
    std::string http_version;
    int status_code = 0;
    response_stream >> http_version >> status_code;
    if (http_version.empty() || status_code <= 0) {
        throw std::runtime_error("could not parse HTTP status from " + host + ":" + port);
    }

    return HttpResponse{
        status_code,
        raw_response.substr(header_end + 4),
    };
}

float parse_float_token(const std::string& token) {
    char* endptr = nullptr;
    const float value = std::strtof(token.c_str(), &endptr);
    if (endptr == token.c_str()) {
        throw std::runtime_error("could not parse float token: " + token);
    }
    return value;
}

std::vector<float> parse_embedding_array(const std::string& body) {
    const size_t key_pos = body.find("\"embedding\"");
    if (key_pos == std::string::npos) {
        throw std::runtime_error("embedding service response did not contain embedding field");
    }

    const size_t array_start = body.find('[', key_pos);
    if (array_start == std::string::npos) {
        throw std::runtime_error("embedding field was malformed");
    }

    size_t pos = array_start + 1;
    int depth = 1;
    size_t array_end = std::string::npos;
    while (pos < body.size()) {
        if (body[pos] == '[') {
            ++depth;
        } else if (body[pos] == ']') {
            --depth;
            if (depth == 0) {
                array_end = pos;
                break;
            }
        }
        ++pos;
    }
    if (array_end == std::string::npos) {
        throw std::runtime_error("embedding array did not terminate");
    }

    std::vector<float> embedding;
    std::string token;
    for (size_t i = array_start + 1; i < array_end; ++i) {
        const char ch = body[i];
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!token.empty()) {
                embedding.push_back(parse_float_token(token));
                token.clear();
            }
            continue;
        }
        token.push_back(ch);
    }
    if (!token.empty()) {
        embedding.push_back(parse_float_token(token));
    }

    if (embedding.empty()) {
        throw std::runtime_error("embedding service returned an empty embedding vector");
    }
    return embedding;
}

int parse_qdrant_count(const std::string& body) {
    const size_t count_pos = body.find("\"count\"");
    if (count_pos == std::string::npos) {
        throw std::runtime_error("Qdrant count response missing count field");
    }
    const size_t colon = body.find(':', count_pos);
    if (colon == std::string::npos) {
        throw std::runtime_error("Qdrant count response malformed");
    }
    size_t value_start = colon + 1;
    while (value_start < body.size() &&
           std::isspace(static_cast<unsigned char>(body[value_start])) != 0) {
        ++value_start;
    }
    size_t value_end = value_start;
    while (value_end < body.size() &&
           std::isdigit(static_cast<unsigned char>(body[value_end])) != 0) {
        ++value_end;
    }
    return std::stoi(body.substr(value_start, value_end - value_start));
}

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open input file " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    std::string text = contents.str();
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) {
        throw std::runtime_error("input file was empty: " + path.string());
    }
    return text;
}

float cosine_similarity(const std::vector<float>& a,
                        const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) {
        return 0.0f;
    }

    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }

    if (norm_a <= 0.0 || norm_b <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>(dot / (std::sqrt(norm_a) * std::sqrt(norm_b)));
}

Config load_config() {
    Config config;

    if (const char* image = std::getenv("EMBEDDING_IMAGE")) {
        config.embedding_image = image;
    } else if (const char* image = std::getenv("INFINITY_IMAGE")) {
        config.embedding_image = image;
    }
    if (const char* model = std::getenv("EMBEDDING_MODEL_ID")) {
        config.model_id = model;
    } else if (const char* model = std::getenv("INFINITY_MODEL_ID")) {
        config.model_id = model;
    }
    if (const char* collection = std::getenv("QDRANT_COLLECTION")) {
        config.collection = collection;
    }
    if (const char* theta = std::getenv("DSCC_THETA")) {
        config.theta = std::strtof(theta, nullptr);
    }
    if (const char* hold = std::getenv("DSCC_LOCK_HOLD_MS")) {
        config.lock_hold_ms = std::atoi(hold);
    }
    if (const char* teardown = std::getenv("E2E_TEARDOWN")) {
        config.teardown_on_exit = std::string(teardown) == "1";
    }
    return config;
}

int run_shell_command(const std::string& command) {
    info("Executing: " + command);
    return std::system(command.c_str());
}

ShellCommandResult run_shell_capture(const std::string& command) {
    const std::string wrapped = command + " 2>&1";
    FILE* pipe = ::popen(wrapped.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to spawn shell command: " + command);
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    const int raw_status = ::pclose(pipe);
    int exit_code = raw_status;
    if (raw_status >= 0 && WIFEXITED(raw_status)) {
        exit_code = WEXITSTATUS(raw_status);
    }

    return ShellCommandResult{exit_code, output};
}

ShellCommandResult run_compose_capture(const Config& config,
                                       const std::string& compose_args) {
    const std::string command =
        "cd " + shell_quote(config.project_root) + " && docker compose " + compose_args;
    return run_shell_capture(command);
}

bool embedding_service_failed(const Config& config,
                              std::string& status_output,
                              std::string& logs_output) {
    const ShellCommandResult status =
        run_compose_capture(config, "ps -a embedding-service");
    const ShellCommandResult logs =
        run_compose_capture(config, "logs --tail=80 embedding-service");

    status_output = status.output;
    logs_output = logs.output;

    const std::string combined = lowercase_copy(status.output + "\n" + logs.output);
    return combined.find("exited") != std::string::npos ||
           combined.find("dead") != std::string::npos ||
           combined.find("restart") != std::string::npos ||
           combined.find("exec format error") != std::string::npos;
}

void start_compose_stack(const Config& config) {
    ::setenv("EMBEDDING_IMAGE", config.embedding_image.c_str(), 1);
    ::setenv("EMBEDDING_MODEL_ID", config.model_id.c_str(), 1);
    ::setenv("INFINITY_IMAGE", config.embedding_image.c_str(), 1);
    ::setenv("INFINITY_MODEL_ID", config.model_id.c_str(), 1);
    ::setenv("QDRANT_COLLECTION", config.collection.c_str(), 1);

    {
        std::ostringstream theta;
        theta << std::fixed << std::setprecision(2) << config.theta;
        const std::string theta_value = theta.str();
        ::setenv("DSCC_THETA", theta_value.c_str(), 1);
    }
    {
        const std::string hold = std::to_string(config.lock_hold_ms);
        ::setenv("DSCC_LOCK_HOLD_MS", hold.c_str(), 1);
    }

    const std::string command =
        "cd " + shell_quote(config.project_root) +
        " && docker compose up -d --build qdrant embedding-service dscc-node";
    if (run_shell_command(command) != 0) {
        throw std::runtime_error("docker compose up failed");
    }
}

void stop_compose_stack(const Config& config) {
    const std::string command =
        "cd " + shell_quote(config.project_root) + " && docker compose down";
    const int code = run_shell_command(command);
    if (code != 0) {
        warn("docker compose down returned non-zero exit code");
    }
}

void wait_for_http_service(const std::string& name,
                           const std::string& host,
                           const std::string& port,
                           const std::vector<std::string>& targets,
                           int max_attempts = 180,
                           int sleep_ms = 2000) {
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        for (const auto& target : targets) {
            try {
                const HttpResponse response =
                    send_http_request(host, port, "GET", target);
                if (response.status_code >= 200 && response.status_code < 300) {
                    info(name + " is reachable on " + host + ":" + port +
                         " via " + target);
                    return;
                }
            } catch (const std::exception&) {
            }
        }
        info("Waiting for " + name + " (" + std::to_string(attempt) + "/" +
             std::to_string(max_attempts) + ")...");
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    throw std::runtime_error("timed out waiting for " + name);
}

void wait_for_embedding_service(const Config& config,
                                int max_attempts = 180,
                                int sleep_ms = 2000) {
    const std::vector<std::string> targets = {"/api/tags", "/v1/models", "/"};
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        for (const auto& target : targets) {
            try {
                const HttpResponse response =
                    send_http_request(config.embedding_host,
                                      config.embedding_port,
                                      "GET",
                                      target);
                if (response.status_code >= 200 && response.status_code < 300) {
                    info("Embedding service is reachable on " + config.embedding_host + ":" +
                         config.embedding_port + " via " + target);
                    return;
                }
            } catch (const std::exception&) {
            }
        }

        if (attempt == 1 || attempt % 3 == 0) {
            std::string status_output;
            std::string logs_output;
            if (embedding_service_failed(config, status_output, logs_output)) {
                throw std::runtime_error(
                    "embedding service container failed before readiness.\n"
                    "Container status:\n" + status_output +
                    "\nContainer logs:\n" + logs_output);
            }
        }

        info("Waiting for embedding service (" + std::to_string(attempt) + "/" +
             std::to_string(max_attempts) + ")...");
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }

    std::string status_output;
    std::string logs_output;
    embedding_service_failed(config, status_output, logs_output);
    throw std::runtime_error(
        "timed out waiting for embedding service.\n"
        "Container status:\n" + status_output +
        "\nContainer logs:\n" + logs_output);
}

void ensure_ollama_model_available(const Config& config) {
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(config.model_id)
         << "\",\"stream\":false}";

    info("Ensuring embedding model is available: " + config.model_id);
    const HttpResponse response =
        send_http_request(config.embedding_host,
                          config.embedding_port,
                          "POST",
                          "/api/pull",
                          body.str());

    if (response.status_code != 200) {
        throw std::runtime_error("embedding model pull failed with status " +
                                 std::to_string(response.status_code));
    }
}

void wait_for_dscc_service(const Config& config,
                           int max_attempts = 90,
                           int sleep_ms = 1000) {
    auto channel = grpc::CreateChannel(config.dscc_target,
                                       grpc::InsecureChannelCredentials());
    auto stub = dscc::LockService::NewStub(channel);

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        grpc::ClientContext context;
        dscc::PingRequest request;
        dscc::PingResponse response;
        request.set_from_node("e2e-bench");
        const grpc::Status status = stub->Ping(&context, request, &response);
        if (status.ok()) {
            info("DSCC gRPC service is reachable at " + config.dscc_target);
            return;
        }
        info("Waiting for DSCC gRPC service (" + std::to_string(attempt) + "/" +
             std::to_string(max_attempts) + ")...");
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    throw std::runtime_error("timed out waiting for DSCC gRPC service");
}

void reset_qdrant_collection(const Config& config) {
    const std::string target = "/collections/" + config.collection;
    const HttpResponse response =
        send_http_request(config.qdrant_host, config.qdrant_port, "DELETE", target);
    if (response.status_code != 200 &&
        response.status_code != 202 &&
        response.status_code != 404) {
        throw std::runtime_error("failed to reset Qdrant collection; status=" +
                                 std::to_string(response.status_code));
    }
}

std::vector<float> request_embedding(const Config& config,
                                     const std::string& text) {
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(config.model_id)
         << "\",\"input\":\"" << escape_json(text) << "\"}";

    const HttpResponse response =
        send_http_request(config.embedding_host,
                          config.embedding_port,
                          "POST",
                          "/v1/embeddings",
                          body.str());

    if (response.status_code != 200) {
        throw std::runtime_error("embedding request failed with status " +
                                 std::to_string(response.status_code));
    }

    return parse_embedding_array(response.body);
}

std::vector<AgentDocument> load_agent_documents(const Config& config) {
    const std::vector<char> labels = {'A', 'B', 'C', 'D', 'E'};
    std::vector<AgentDocument> documents;
    documents.reserve(labels.size());

    for (char label : labels) {
        AgentDocument doc;
        doc.label = label;
        doc.source_file = std::string(1, label) + ".txt";
        doc.text = read_text_file(fs::path(config.input_dir) / doc.source_file);
        info("Embedding " + doc.source_file + " via embedding service...");
        doc.embedding = request_embedding(config, doc.text);
        info(doc.source_file + " embedding dimension = " +
             std::to_string(doc.embedding.size()));
        documents.push_back(std::move(doc));
    }

    return documents;
}

std::map<char, AgentDocument> index_documents(const std::vector<AgentDocument>& docs) {
    std::map<char, AgentDocument> indexed;
    for (const auto& doc : docs) {
        indexed.emplace(doc.label, doc);
    }
    return indexed;
}

void print_similarity_matrix(const std::map<char, AgentDocument>& docs) {
    section_header("Pairwise Similarity Matrix");

    std::ostringstream header;
    header << std::setw(8) << "";
    for (const auto& [label, _] : docs) {
        header << std::setw(8) << label;
    }
    log_line(header.str());

    for (const auto& [label_a, doc_a] : docs) {
        std::ostringstream row;
        row << std::setw(8) << label_a;
        for (const auto& [label_b, doc_b] : docs) {
            row << std::setw(8)
                << format_float(cosine_similarity(doc_a.embedding, doc_b.embedding));
        }
        log_line(row.str());
    }
}

int qdrant_point_count(const Config& config) {
    const HttpResponse response =
        send_http_request(config.qdrant_host,
                          config.qdrant_port,
                          "POST",
                          "/collections/" + config.collection + "/points/count",
                          "{\"exact\":true}");
    if (response.status_code != 200) {
        throw std::runtime_error("Qdrant count request failed with status " +
                                 std::to_string(response.status_code));
    }
    return parse_qdrant_count(response.body);
}

std::string qdrant_scroll_payloads(const Config& config) {
    const HttpResponse response =
        send_http_request(config.qdrant_host,
                          config.qdrant_port,
                          "POST",
                          "/collections/" + config.collection + "/points/scroll",
                          "{\"limit\":100,\"with_payload\":true,\"with_vector\":false}");
    if (response.status_code != 200) {
        throw std::runtime_error("Qdrant scroll request failed with status " +
                                 std::to_string(response.status_code));
    }
    return response.body;
}

void create_qdrant_collection(const Config& config, size_t vector_size) {
    std::ostringstream body;
    body << "{\"vectors\":{\"size\":" << vector_size
         << ",\"distance\":\"Cosine\"}}";

    const HttpResponse response =
        send_http_request(config.qdrant_host,
                          config.qdrant_port,
                          "PUT",
                          "/collections/" + config.collection,
                          body.str());

    if (response.status_code != 200 &&
        response.status_code != 201 &&
        response.status_code != 409) {
        throw std::runtime_error("failed to create Qdrant collection; status=" +
                                 std::to_string(response.status_code) +
                                 " body=" + response.body);
    }
}

bool validate_qdrant_payloads(const std::string& scroll_body,
                              const std::vector<AgentOutcome>& outcomes,
                              const std::map<char, AgentDocument>& docs) {
    for (const auto& outcome : outcomes) {
        if (scroll_body.find(outcome.agent_id) == std::string::npos) {
            error("Qdrant payload check failed: missing agent_id " + outcome.agent_id);
            return false;
        }

        const auto& doc = docs.at(outcome.label);
        if (scroll_body.find(doc.source_file) == std::string::npos) {
            error("Qdrant payload check failed: missing source_file " + doc.source_file);
            return false;
        }
        if (scroll_body.find(doc.text) == std::string::npos) {
            error("Qdrant payload check failed: missing raw text from " + doc.source_file);
            return false;
        }
    }
    return true;
}

std::string scenario_slug(const std::string& name) {
    std::string slug;
    slug.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            slug.push_back(static_cast<char>(std::tolower(c)));
        } else {
            slug.push_back('-');
        }
    }
    return slug;
}

int64_t relative_unix_ms(int64_t unix_ms, int64_t unix_base_ms) {
    if (unix_ms <= 0) {
        return -1;
    }
    return unix_ms - unix_base_ms;
}

std::string format_optional_ms(int64_t value) {
    if (value < 0) {
        return "n/a";
    }
    return std::to_string(value) + "ms";
}

void print_timeline(const std::vector<AgentOutcome>& outcomes,
                    int64_t unix_base_ms,
                    float theta) {
    std::vector<TimelineEvent> events;
    size_t sequence = 0;
    auto push_event = [&](int64_t time_ms,
                          const char* color,
                          const std::string& text) {
        if (time_ms < 0) {
            return;
        }
        events.push_back(TimelineEvent{time_ms, sequence++, color, text});
    };

    for (const auto& outcome : outcomes) {
        push_event(outcome.submit_ms,
                   ansi::kBlue,
                   outcome.display_name + " submitted text");
        push_event(outcome.embedding_start_ms,
                   ansi::kCyan,
                   outcome.display_name + " entered embedding model");
        push_event(outcome.embedding_finish_ms,
                   ansi::kCyan,
                   outcome.display_name + " left embedding model");
        push_event(outcome.dslm_enter_ms,
                   ansi::kYellow,
                   outcome.display_name + " entered DSLM");
        push_event(relative_unix_ms(outcome.response.server_received_unix_ms(), unix_base_ms),
                   ansi::kYellow,
                   outcome.display_name + " reached DSLM server");

        std::ostringstream acquired;
        acquired << outcome.display_name
                 << " acquired semantic lock";
        if (outcome.response.lock_wait_ms() > 0) {
            acquired << " after waiting " << outcome.response.lock_wait_ms() << "ms";
            if (!outcome.response.blocking_agent_id().empty()) {
                acquired << " because "
                         << display_agent_id(outcome.response.blocking_agent_id())
                         << " matched at similarity "
                         << format_float(outcome.response.blocking_similarity_score())
                         << " >= theta " << format_float(theta);
            }
        }
        push_event(relative_unix_ms(outcome.response.lock_acquired_unix_ms(), unix_base_ms),
                   outcome.response.lock_wait_ms() > 0 ? ansi::kYellow : ansi::kGreen,
                   acquired.str());

        std::ostringstream written;
        written << outcome.display_name << " finished Qdrant write";
        push_event(relative_unix_ms(outcome.response.qdrant_write_complete_unix_ms(),
                                    unix_base_ms),
                   ansi::kGreen,
                   written.str());

        std::ostringstream released;
        released << outcome.display_name << " released semantic lock";
        push_event(relative_unix_ms(outcome.response.lock_released_unix_ms(), unix_base_ms),
                   ansi::kGreen,
                   released.str());
    }

    std::sort(events.begin(), events.end(), [](const TimelineEvent& left,
                                               const TimelineEvent& right) {
        if (left.time_ms != right.time_ms) {
            return left.time_ms < right.time_ms;
        }
        return left.sequence < right.sequence;
    });

    log_line("Timeline:");
    for (const auto& event : events) {
        std::ostringstream line;
        line << "  t+" << std::setw(4) << event.time_ms << "ms  " << event.text;
        log_line(std::string(event.color) + line.str() + ansi::kReset);
    }
}

void print_agent_recaps(const std::vector<AgentOutcome>& outcomes,
                        int64_t unix_base_ms) {
    log_line("");
    log_line("Agent Recap:");
    for (const auto& outcome : outcomes) {
        log_line(std::string(ansi::kMagenta) + "  " + outcome.display_name + ansi::kReset);

        std::ostringstream line1;
        line1 << "    embedding: "
              << (outcome.embedding_finish_ms - outcome.embedding_start_ms) << "ms";
        log_line(line1.str());

        std::ostringstream line2;
        line2 << "    DSLM wait: " << outcome.response.lock_wait_ms() << "ms";
        if (!outcome.response.blocking_agent_id().empty()) {
            line2 << " due to " << display_agent_id(outcome.response.blocking_agent_id())
                  << " (similarity "
                  << format_float(outcome.response.blocking_similarity_score()) << ")";
        }
        log_line(line2.str());

        std::ostringstream line3;
        line3 << "    write complete: "
              << format_optional_ms(relative_unix_ms(
                     outcome.response.qdrant_write_complete_unix_ms(), unix_base_ms));
        log_line(line3.str());

        std::ostringstream line4;
        line4 << "    result: "
              << (outcome.response.granted() ? "granted" : "failed")
              << " in " << outcome.elapsed_ms << "ms";
        log_line(line4.str());

        log_line("");
    }
}

bool validate_conflict_pairs(const Scenario& scenario,
                             const std::vector<AgentOutcome>& outcomes,
                             const std::map<char, AgentDocument>& docs,
                             const Config& config) {
    std::map<char, AgentOutcome> indexed;
    for (const auto& outcome : outcomes) {
        indexed[outcome.label] = outcome;
    }

    for (const auto& [left, right] : scenario.expected_conflicts) {
        const float similarity =
            cosine_similarity(docs.at(left).embedding, docs.at(right).embedding);
        if (similarity < config.theta) {
            error("Expected conflict pair " + std::string(1, left) + "/" +
                  std::string(1, right) + " did not exceed theta; score=" +
                  format_float(similarity) + " theta=" + format_float(config.theta));
            return false;
        }

        const int64_t finish_gap =
            std::llabs(indexed.at(left).finish_ms - indexed.at(right).finish_ms);
        const int64_t minimum_gap = std::max<int64_t>(250, config.lock_hold_ms / 2);
        if (finish_gap < minimum_gap) {
            error("Conflict pair " + std::string(1, left) + "/" +
                  std::string(1, right) +
                  " completed too close together to prove serialization; finish_gap=" +
                  std::to_string(finish_gap) + "ms expected_at_least=" +
                  std::to_string(minimum_gap) + "ms");
            return false;
        }
    }

    return true;
}

bool validate_distinct_completion(const std::vector<AgentOutcome>& outcomes,
                                  const Config& config) {
    int64_t earliest = outcomes.front().finish_ms;
    int64_t latest = outcomes.front().finish_ms;
    for (const auto& outcome : outcomes) {
        earliest = std::min(earliest, outcome.finish_ms);
        latest = std::max(latest, outcome.finish_ms);
    }

    const int64_t spread = latest - earliest;
    const int64_t max_spread = std::max<int64_t>(300, config.lock_hold_ms / 2);
    if (spread > max_spread) {
        error("Distinct-task scenario spread was too large; finish spread=" +
              std::to_string(spread) + "ms expected_at_most=" +
              std::to_string(max_spread) + "ms");
        return false;
    }
    return true;
}

bool scenario_has_threshold_conflict(const Scenario& scenario,
                                     const std::map<char, AgentDocument>& docs,
                                     const Config& config) {
    for (size_t i = 0; i < scenario.labels.size(); ++i) {
        for (size_t j = i + 1; j < scenario.labels.size(); ++j) {
            const float similarity =
                cosine_similarity(docs.at(scenario.labels[i]).embedding,
                                  docs.at(scenario.labels[j]).embedding);
            if (similarity >= config.theta) {
                return true;
            }
        }
    }
    return false;
}

bool run_scenario(const Config& config,
                  const Scenario& scenario,
                  const std::map<char, AgentDocument>& docs) {
    section_header(scenario.name);
    info("Preparing fresh Qdrant collection.");
    reset_qdrant_collection(config);

    std::vector<AgentDocument> selected_docs;
    selected_docs.reserve(scenario.labels.size());
    for (char label : scenario.labels) {
        selected_docs.push_back(docs.at(label));
    }
    create_qdrant_collection(config, selected_docs.front().embedding.size());
    wait_for_http_service("Qdrant collection", config.qdrant_host, config.qdrant_port,
                          {"/collections/" + config.collection}, 30, 200);

    std::vector<AgentOutcome> outcomes(selected_docs.size());
    std::vector<std::thread> threads;
    threads.reserve(selected_docs.size());
    std::barrier sync_point(static_cast<std::ptrdiff_t>(selected_docs.size()));

    auto channel = grpc::CreateChannel(config.dscc_target,
                                       grpc::InsecureChannelCredentials());
    const auto scenario_start = SteadyClock::now();
    const auto now_ms = [&]() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   SteadyClock::now() - scenario_start)
            .count();
    };
    const int64_t unix_base_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    for (size_t i = 0; i < selected_docs.size(); ++i) {
        threads.emplace_back([&, i]() {
            auto stub = dscc::LockService::NewStub(channel);
            const auto& doc = selected_docs[i];
            outcomes[i].label = doc.label;
            outcomes[i].display_name = agent_name(doc.label);
            outcomes[i].agent_id = "scenario-" + scenario_slug(scenario.name) +
                                   "-agent-" + doc.label;

            sync_point.arrive_and_wait();

            outcomes[i].submit_ms = now_ms();
            outcomes[i].embedding_start_ms = now_ms();
            outcomes[i].embedding = request_embedding(config, doc.text);
            outcomes[i].embedding_finish_ms = now_ms();

            dscc::AcquireRequest request;
            request.set_agent_id(outcomes[i].agent_id);
            request.set_payload_text(doc.text);
            request.set_source_file(doc.source_file);
            request.set_timestamp_unix_ms(unix_base_ms + static_cast<int64_t>(i));
            for (float value : outcomes[i].embedding) {
                request.add_embedding(value);
            }

            grpc::ClientContext context;
            outcomes[i].dslm_enter_ms = now_ms();
            outcomes[i].status = stub->AcquireGuard(&context, request, &outcomes[i].response);
            outcomes[i].dslm_exit_ms = now_ms();
            outcomes[i].finish_ms = outcomes[i].dslm_exit_ms;
            outcomes[i].elapsed_ms = outcomes[i].finish_ms - outcomes[i].submit_ms;
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    print_timeline(outcomes, unix_base_ms, config.theta);
    print_agent_recaps(outcomes, unix_base_ms);

    for (const auto& outcome : outcomes) {
        if (!outcome.status.ok()) {
            error(outcome.agent_id + " gRPC failed: " + outcome.status.error_message());
            return false;
        }
        if (!outcome.response.granted()) {
            error(outcome.agent_id + " was not granted by DSCC: " +
                  outcome.response.message());
            return false;
        }
    }

    const int count = qdrant_point_count(config);
    if (count != static_cast<int>(selected_docs.size())) {
        error("Qdrant count mismatch; expected " + std::to_string(selected_docs.size()) +
              " points, got " + std::to_string(count));
        return false;
    }
    success("Qdrant point count matched expected value " + std::to_string(count));

    const std::string scroll_body = qdrant_scroll_payloads(config);
    if (!validate_qdrant_payloads(scroll_body, outcomes, docs)) {
        return false;
    }
    success("Qdrant payload metadata check passed.");

    if (!validate_conflict_pairs(scenario, outcomes, docs, config)) {
        return false;
    }
    if (scenario.expect_distinct_completion) {
        if (!scenario_has_threshold_conflict(scenario, docs, config)) {
            if (!validate_distinct_completion(outcomes, config)) {
                return false;
            }
        } else {
            warn("This scenario is no longer distinct at the current theta, so the distinct-timing check was skipped.");
        }
    }

    success("Scenario passed.");
    return true;
}

void print_runtime_summary(const Config& config) {
    section_header("Runtime Configuration");
    info("Project root: " + config.project_root);
    info("Input directory: " + config.input_dir);
    info("Embedding image: " + config.embedding_image);
    info("Embedding model: " + config.model_id);
    info("DSCC theta: " + format_float(config.theta));
    info("DSCC lock hold: " + std::to_string(config.lock_hold_ms) + "ms");
    info("Qdrant collection: " + config.collection);
}

}  // namespace

int main() {
    bool stack_started = false;
    Config config;
    try {
        config = load_config();
        print_runtime_summary(config);

        start_compose_stack(config);
        stack_started = true;

        wait_for_http_service("Qdrant",
                              config.qdrant_host,
                              config.qdrant_port,
                              {"/collections", "/"});
        wait_for_embedding_service(config);
        ensure_ollama_model_available(config);
        wait_for_dscc_service(config);

        const std::vector<AgentDocument> documents = load_agent_documents(config);
        const std::map<char, AgentDocument> indexed_docs = index_documents(documents);
        print_similarity_matrix(indexed_docs);

        const std::vector<Scenario> scenarios = {
            {"Scenario One: Agents A and B describe the same invoice", {'A', 'B'},
             {{'A', 'B'}}, false},
            {"Scenario Two: Agents A, C, and D run together", {'A', 'C', 'D'}, {}, true},
            {"Scenario Three: Agents D and E describe the same payroll task", {'D', 'E'},
             {{'D', 'E'}}, false},
            {"Scenario Four: Full fan-in with Agents A through E", {'A', 'B', 'C', 'D', 'E'},
             {{'A', 'B'}, {'D', 'E'}}, false},
        };

        bool overall_pass = true;
        for (const auto& scenario : scenarios) {
            const bool pass = run_scenario(config, scenario, indexed_docs);
            overall_pass = overall_pass && pass;
        }

        section_header("End-to-End Summary");
        if (overall_pass) {
            success("All real end-to-end scenarios passed.");
            if (config.teardown_on_exit) {
                stop_compose_stack(config);
            }
            if (!config.teardown_on_exit) {
                info("Docker stack was left running for inspection. Use `docker compose down` when finished.");
            }
            return 0;
        }

        if (config.teardown_on_exit) {
            stop_compose_stack(config);
        }
        error("One or more end-to-end scenarios failed.");
        return 1;
    } catch (const std::exception& ex) {
        if (stack_started && config.teardown_on_exit) {
            stop_compose_stack(config);
        }
        error(std::string("Fatal error: ") + ex.what());
        return 1;
    } catch (...) {
        if (stack_started && config.teardown_on_exit) {
            stop_compose_stack(config);
        }
        error("Fatal error: unknown exception");
        return 1;
    }
}
