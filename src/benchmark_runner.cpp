// Runs a curated semantic-lock benchmark suite against the full DSCC stack.
// The runner prints a readable ANSI timeline and streams raw per-case JSON.

#include "dscc.grpc.pb.h"
#include "dscc_raft.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <netdb.h>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

namespace ansi {
constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kBlue = "\033[34m";
constexpr const char* kMagenta = "\033[35m";
constexpr const char* kWhite = "\033[37m";
constexpr const char* kGray = "\033[90m";
}  // namespace ansi

struct Config {
    std::string project_root = DSLM_PROJECT_ROOT;
    std::string input_dir = DSLM_PROJECT_ROOT "/demo_inputs";
    std::string embedding_image = "ollama/ollama:latest";
    std::string model_id = "all-minilm:latest";
    std::string collection_prefix = "dscc_curated";
    std::string embedding_host = "127.0.0.1";
    std::string embedding_port = "7997";
    std::string qdrant_host = "127.0.0.1";
    std::string qdrant_port = "6333";
    std::string dscc_target = "127.0.0.1:50050";
    std::vector<std::string> node_targets = {
        "127.0.0.1:50051",
        "127.0.0.1:50052",
        "127.0.0.1:50053",
        "127.0.0.1:50054",
        "127.0.0.1:50055",
    };
    std::vector<std::string> node_service_names = {
        "dscc-node-1",
        "dscc-node-2",
        "dscc-node-3",
        "dscc-node-4",
        "dscc-node-5",
    };
    bool teardown_on_exit = true;
    std::string output_path;
};

struct HttpResponse {
    int status_code = 0;
    std::string body;
};

struct ShellCommandResult {
    int exit_code = 0;
    std::string output;
};

struct LeaderObservation {
    std::string node_target;
    std::string service_name;
    std::string leader_address;
    std::string leader_id;
    int64_t current_term = 0;
};

struct TemplateDocument {
    std::string template_id;
    std::string source_file;
    std::string text;
    std::vector<float> embedding;
};

enum class OperationType {
    kWrite,
    kRead,
};

enum class ArrivalMode {
    kBurst,
    kStaggered,
};

enum class ScenarioKind {
    kThunderingHerd,
    kSemanticInterleaving,
    kReadStarvationTrap,
    kPermissiveSieve,
    kStrictSieve,
    kGhostClient,
    kAlmostCollision,
    kQueueHopping,
    kMixedStagger,
    kReadStampede,
};

struct BenchmarkCase {
    int case_index = 0;
    ScenarioKind kind = ScenarioKind::kThunderingHerd;
    std::string name;
    std::string target;
    int node_count = 5;
    int agent_count = 0;
    int write_count = 0;
    int read_count = 0;
    float theta = 0.55f;
    int lock_hold_ms = 0;
    ArrivalMode arrival_mode = ArrivalMode::kBurst;
    int arrival_gap_ms = 0;
    std::string collection_name;
};

struct BenchmarkOperation {
    std::string agent_id;
    std::string role_prefix;
    std::string template_id;
    std::string text;
    std::vector<float> embedding;
    OperationType operation = OperationType::kWrite;
    int64_t scheduled_offset_ms = 0;
};

struct OperationResult {
    BenchmarkOperation operation;
    grpc::Status status;
    dscc::AcquireResponse response;
    int64_t submit_ms = 0;
    int64_t dslm_enter_ms = 0;
    int64_t dslm_exit_ms = 0;
    int64_t finish_ms = 0;
    int64_t elapsed_ms = 0;
};

struct ContainerStats {
    std::string name;
    double cpu_percent = 0.0;
    double memory_used_mib = 0.0;
    double net_input_mib = 0.0;
    double net_output_mib = 0.0;
};

struct BenchmarkMetrics {
    int total_ops = 0;
    int write_ops = 0;
    int read_ops = 0;
    int grpc_failures = 0;
    int granted_ops = 0;
    int blocked_ops = 0;
    int blocked_writes = 0;
    int blocked_reads = 0;
    int blocked_reads_on_write = 0;
    int expected_conflict_pairs = 0;
    int expected_distinct_pairs = 0;
    int conflicting_overlap_violations = 0;
    int distinct_parallel_pairs = 0;
    int distinct_nonparallel_pairs = 0;
    int active_lock_count_max = 0;
    int wait_position_max = 0;
    int wake_count_max = 0;
    int queue_hops_max = 0;
    double success_rate = 0.0;
    double throughput_ops_per_sec = 0.0;
    double serialization_score = 1.0;
    double distinct_parallelism_rate = 0.0;
    int64_t makespan_ms = 0;
    int64_t latency_p50_ms = 0;
    int64_t latency_p95_ms = 0;
    int64_t latency_p99_ms = 0;
    int64_t write_latency_p95_ms = 0;
    int64_t read_latency_p95_ms = 0;
    int64_t lock_wait_p50_ms = 0;
    int64_t lock_wait_p95_ms = 0;
    int64_t lock_wait_p99_ms = 0;
    int64_t qdrant_window_p95_ms = 0;
    int64_t queue_position_p95 = 0;
    int64_t wake_count_p95 = 0;
    int64_t queue_hops_p95 = 0;
};

struct BenchmarkCaseResult {
    BenchmarkCase spec;
    BenchmarkMetrics metrics;
    std::vector<OperationResult> operations;
    std::vector<ContainerStats> container_stats;
};

struct ViolationRecord {
    std::string active_agent_id;
    std::string violating_agent_id;
    OperationType active_operation = OperationType::kWrite;
    OperationType violating_operation = OperationType::kWrite;
    float similarity = 0.0f;
    int64_t detected_unix_ms = 0;
    int64_t overlap_start_unix_ms = 0;
    int64_t overlap_end_unix_ms = 0;
};

struct TemplateCatalog {
    const TemplateDocument* concept_a = nullptr;
    const TemplateDocument* concept_b = nullptr;
    std::vector<const TemplateDocument*> all;
};

struct TimelineLine {
    int64_t absolute_ms = 0;
    size_t sequence = 0;
    const char* color = ansi::kWhite;
    std::string text;
};

std::vector<ViolationRecord> compute_violation_records(const BenchmarkCase& spec,
                                                       const std::vector<OperationResult>& operations);

std::string lowercase_copy(std::string text) {
    std::transform(text.begin(),
                   text.end(),
                   text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

std::string trim_copy(const std::string& input) {
    size_t begin = 0;
    while (begin < input.size() &&
           std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    size_t end = input.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(begin, end - begin);
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

void info(const std::string& message) {
    std::cout << "[INFO] " << message << std::endl;
}

void warn(const std::string& message) {
    std::cout << "[WARN] " << message << std::endl;
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string operation_name(OperationType operation) {
    return operation == OperationType::kRead ? "read" : "write";
}

std::string operation_title(OperationType operation) {
    return operation == OperationType::kRead ? "Read" : "Write";
}

dscc::AcquireRequest::OperationType to_proto_operation(OperationType operation) {
    return operation == OperationType::kRead
               ? dscc::AcquireRequest::OPERATION_TYPE_READ
               : dscc::AcquireRequest::OPERATION_TYPE_WRITE;
}

std::string arrival_mode_label(const BenchmarkCase& spec) {
    if (spec.arrival_mode == ArrivalMode::kBurst) {
        return "Burst";
    }
    std::ostringstream out;
    out << "Staggered (" << spec.arrival_gap_ms << "ms)";
    return out.str();
}

std::string slugify(std::string text) {
    std::string output;
    output.reserve(text.size());
    for (char c : text) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (std::isalnum(ch) != 0) {
            output.push_back(static_cast<char>(std::tolower(ch)));
        } else if (c == ' ' || c == '-' || c == '_') {
            if (output.empty() || output.back() == '_') {
                continue;
            }
            output.push_back('_');
        }
    }
    while (!output.empty() && output.back() == '_') {
        output.pop_back();
    }
    return output;
}

std::string role_prefix_for_source_file(const std::string& source_file) {
    if (source_file == "A.json") {
        return "sustainability_agent";
    }
    if (source_file == "B.json") {
        return "safety_agent";
    }
    if (source_file == "C.json") {
        return "cost_agent";
    }
    if (source_file == "D.json") {
        return "construction_agent";
    }
    if (source_file == "E.json") {
        return "client_agent";
    }
    return "agent";
}

std::string getenv_or_default(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value != nullptr ? value : fallback;
}

bool getenv_flag(const char* key, bool fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr) {
        return fallback;
    }
    const std::string normalized = lowercase_copy(value);
    return normalized == "1" || normalized == "true" || normalized == "yes";
}

Config load_config() {
    Config config;
    config.embedding_image = getenv_or_default("EMBEDDING_IMAGE", config.embedding_image);
    config.model_id = getenv_or_default("EMBEDDING_MODEL_ID", config.model_id);
    config.collection_prefix = getenv_or_default("DSLM_BENCH_COLLECTION_PREFIX",
                                                 config.collection_prefix);
    config.teardown_on_exit = getenv_flag("E2E_TEARDOWN", true);
    config.output_path = getenv_or_default("DSLM_BENCH_OUTPUT", "");
    return config;
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
    const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
    if (rc != 0) {
        fail("DNS resolution failed for " + host + ":" + port);
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
        fail("connect failed for " + host + ":" + port);
    }

    std::ostringstream request;
    request << method << " " << target << " HTTP/1.1\r\n";
    request << "Host: " << host << ":" << port << "\r\n";
    request << "Connection: close\r\n";
    if (!body.empty()) {
        request << "Content-Type: " << content_type << "\r\n";
        request << "Content-Length: " << body.size() << "\r\n";
    }
    request << "\r\n" << body;

    if (!send_all(socket_fd, request.str())) {
        ::close(socket_fd);
        fail("send failed for " + host + ":" + port);
    }

    std::string raw;
    char buffer[4096];
    while (true) {
        const ssize_t received = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received < 0) {
            ::close(socket_fd);
            fail("recv failed for " + host + ":" + port);
        }
        if (received == 0) {
            break;
        }
        raw.append(buffer, static_cast<size_t>(received));
    }
    ::close(socket_fd);

    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        fail("malformed HTTP response from " + host + ":" + port);
    }

    std::istringstream stream(raw.substr(0, header_end));
    std::string http_version;
    int status_code = 0;
    stream >> http_version >> status_code;
    if (http_version.empty() || status_code <= 0) {
        fail("could not parse HTTP status from " + host + ":" + port);
    }

    return HttpResponse{status_code, raw.substr(header_end + 4)};
}

float parse_float_token(const std::string& token) {
    char* endptr = nullptr;
    const float value = std::strtof(token.c_str(), &endptr);
    if (endptr == token.c_str()) {
        fail("could not parse float token: " + token);
    }
    return value;
}

std::vector<float> parse_embedding_array(const std::string& body) {
    const size_t key_pos = body.find("\"embedding\"");
    if (key_pos == std::string::npos) {
        fail("embedding response missing embedding field");
    }
    const size_t array_start = body.find('[', key_pos);
    if (array_start == std::string::npos) {
        fail("embedding field malformed");
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
        fail("embedding array did not terminate");
    }

    std::vector<float> values;
    std::string token;
    for (size_t i = array_start + 1; i < array_end; ++i) {
        const char ch = body[i];
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!token.empty()) {
                values.push_back(parse_float_token(token));
                token.clear();
            }
            continue;
        }
        token.push_back(ch);
    }
    if (!token.empty()) {
        values.push_back(parse_float_token(token));
    }
    if (values.empty()) {
        fail("embedding response returned empty vector");
    }
    return values;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        fail("could not open file " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    std::string text = contents.str();
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) {
        fail("file was empty: " + path.string());
    }
    return text;
}

std::string parse_json_string_field(const std::string& body, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    const size_t key_pos = body.find(quoted_key);
    if (key_pos == std::string::npos) {
        fail("missing JSON string field: " + key);
    }
    const size_t colon = body.find(':', key_pos + quoted_key.size());
    const size_t quote = body.find('"', colon + 1);
    if (colon == std::string::npos || quote == std::string::npos) {
        fail("malformed JSON string field: " + key);
    }

    std::string value;
    bool escaping = false;
    for (size_t i = quote + 1; i < body.size(); ++i) {
        const char ch = body[i];
        if (escaping) {
            value.push_back(ch == 'n' ? '\n' : ch);
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    fail("unterminated JSON string field: " + key);
}

std::vector<std::string> parse_payload_schedule_entries(const std::string& body) {
    const std::string quoted_key = "\"payload_schedule\"";
    const size_t key_pos = body.find(quoted_key);
    if (key_pos == std::string::npos) {
        return {};
    }
    const size_t array_start = body.find('[', key_pos + quoted_key.size());
    if (array_start == std::string::npos) {
        fail("malformed payload_schedule array");
    }

    std::vector<std::string> entries;
    bool in_string = false;
    bool escaping = false;
    int object_depth = 0;
    size_t object_start = std::string::npos;
    for (size_t i = array_start + 1; i < body.size(); ++i) {
        const char ch = body[i];
        if (escaping) {
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (ch == '{') {
            if (object_depth == 0) {
                object_start = i;
            }
            ++object_depth;
            continue;
        }
        if (ch == '}') {
            if (object_depth > 0) {
                --object_depth;
                if (object_depth == 0 && object_start != std::string::npos) {
                    entries.push_back(body.substr(object_start, i - object_start + 1));
                }
            }
            continue;
        }
        if (ch == ']' && object_depth == 0) {
            break;
        }
    }
    return entries;
}

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
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
    const double value = dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
    return static_cast<float>(std::max(-1.0, std::min(1.0, value)));
}

ShellCommandResult run_shell_capture(const std::string& command) {
    const std::string wrapped = command + " 2>&1";
    FILE* pipe = ::popen(wrapped.c_str(), "r");
    if (pipe == nullptr) {
        fail("failed to spawn shell command: " + command);
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

void run_shell_or_throw(const std::string& command, const std::string& failure_message) {
    info("Executing: " + command);
    const ShellCommandResult result = run_shell_capture(command);
    if (result.exit_code != 0) {
        fail(failure_message + "\n" + result.output);
    }
}

std::string compose_command(const Config& config, const std::string& args) {
    return "cd " + shell_quote(config.project_root) + " && docker compose " + args;
}

void wait_for_http_service(const std::string& name,
                           const std::string& host,
                           const std::string& port,
                           const std::vector<std::string>& targets,
                           int max_attempts = 120,
                           int sleep_ms = 1000) {
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        for (const auto& target : targets) {
            try {
                const HttpResponse response = send_http_request(host, port, "GET", target);
                if (response.status_code >= 200 && response.status_code < 300) {
                    return;
                }
            } catch (const std::exception&) {
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    fail("timed out waiting for " + name);
}

void wait_for_embedding_service(const Config& config) {
    wait_for_http_service("embedding service",
                          config.embedding_host,
                          config.embedding_port,
                          {"/api/tags", "/v1/models", "/"});
}

void ensure_ollama_model_available(const Config& config) {
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(config.model_id)
         << "\",\"stream\":false}";
    const HttpResponse response =
        send_http_request(config.embedding_host,
                          config.embedding_port,
                          "POST",
                          "/api/pull",
                          body.str());
    if (response.status_code != 200) {
        fail("embedding model pull failed with status " + std::to_string(response.status_code));
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
        request.set_from_node("benchmark-runner");
        const grpc::Status status = stub->Ping(&context, request, &response);
        if (status.ok()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    fail("timed out waiting for DSCC proxy");
}

std::optional<LeaderObservation> discover_leader(const std::vector<std::string>& targets,
                                                 const Config& config) {
    for (size_t i = 0; i < targets.size(); ++i) {
        auto channel = grpc::CreateChannel(targets[i], grpc::InsecureChannelCredentials());
        auto stub = dscc_raft::RaftService::NewStub(channel);
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        dscc_raft::LeaderQuery request;
        dscc_raft::LeaderInfo response;
        const grpc::Status status = stub->GetLeader(&context, request, &response);
        if (!status.ok()) {
            continue;
        }
        if (!response.leader_address().empty()) {
            LeaderObservation observation;
            observation.node_target = targets[i];
            observation.leader_address = response.leader_address();
            observation.leader_id = response.leader_id();
            observation.current_term = response.current_term();
            for (size_t j = 0; j < config.node_targets.size(); ++j) {
                if (config.node_targets[j] == targets[i]) {
                    observation.service_name = config.node_service_names[j];
                    break;
                }
            }
            return observation;
        }
    }
    return std::nullopt;
}

LeaderObservation wait_for_leader(const std::vector<std::string>& targets,
                                  const Config& config,
                                  std::chrono::milliseconds timeout) {
    const auto deadline = SteadyClock::now() + timeout;
    while (SteadyClock::now() < deadline) {
        const std::optional<LeaderObservation> leader = discover_leader(targets, config);
        if (leader.has_value()) {
            return *leader;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fail("timed out waiting for a leader");
}

void reset_qdrant_collection(const Config& config, const std::string& collection_name) {
    const HttpResponse response = send_http_request(config.qdrant_host,
                                                    config.qdrant_port,
                                                    "DELETE",
                                                    "/collections/" + collection_name);
    if (response.status_code != 200 &&
        response.status_code != 202 &&
        response.status_code != 404) {
        fail("failed to reset Qdrant collection");
    }
}

void create_qdrant_collection(const Config& config,
                              const std::string& collection_name,
                              size_t vector_size) {
    std::ostringstream body;
    body << "{\"vectors\":{\"size\":" << vector_size
         << ",\"distance\":\"Cosine\"}}";
    const HttpResponse response =
        send_http_request(config.qdrant_host,
                          config.qdrant_port,
                          "PUT",
                          "/collections/" + collection_name,
                          body.str());
    if (response.status_code != 200 &&
        response.status_code != 201 &&
        response.status_code != 409) {
        fail("failed to create Qdrant collection: " + response.body);
    }
}

int qdrant_point_count(const Config& config, const std::string& collection_name) {
    const HttpResponse response =
        send_http_request(config.qdrant_host,
                          config.qdrant_port,
                          "POST",
                          "/collections/" + collection_name + "/points/count",
                          "{\"exact\":true}");
    if (response.status_code != 200) {
        fail("Qdrant count request failed");
    }
    const size_t count_pos = response.body.find("\"count\"");
    const size_t colon = response.body.find(':', count_pos);
    size_t value_start = colon + 1;
    while (value_start < response.body.size() &&
           std::isspace(static_cast<unsigned char>(response.body[value_start])) != 0) {
        ++value_start;
    }
    size_t value_end = value_start;
    while (value_end < response.body.size() &&
           std::isdigit(static_cast<unsigned char>(response.body[value_end])) != 0) {
        ++value_end;
    }
    return std::stoi(response.body.substr(value_start, value_end - value_start));
}

std::vector<float> request_embedding(const Config& config, const std::string& text) {
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
        fail("embedding request failed with status " + std::to_string(response.status_code));
    }
    return parse_embedding_array(response.body);
}

std::vector<TemplateDocument> load_templates(const Config& config) {
    std::set<std::string> unique_texts;
    std::vector<TemplateDocument> templates;
    int counter = 0;
    for (const char label : std::string("ABCDE")) {
        const fs::path json_path = fs::path(config.input_dir) / (std::string(1, label) + ".json");
        if (!fs::exists(json_path)) {
            continue;
        }
        const std::string json = read_text_file(json_path);
        std::vector<std::string> entries = parse_payload_schedule_entries(json);
        if (entries.empty()) {
            entries.push_back(json);
        }
        for (const std::string& entry : entries) {
            const std::string payload = parse_json_string_field(entry, "payload");
            if (!unique_texts.insert(payload).second) {
                continue;
            }
            TemplateDocument document;
            document.template_id = "tpl-" + std::to_string(++counter);
            document.source_file = json_path.filename().string();
            document.text = payload;
            document.embedding = request_embedding(config, document.text);
            templates.push_back(std::move(document));
        }
    }
    if (templates.size() < 10) {
        fail("need at least ten unique template texts for curated benchmarking");
    }
    return templates;
}

const TemplateDocument* find_first_template_from_source(const std::vector<TemplateDocument>& templates,
                                                        const std::string& source_file) {
    for (const TemplateDocument& document : templates) {
        if (document.source_file == source_file) {
            return &document;
        }
    }
    return nullptr;
}

std::vector<const TemplateDocument*> pick_unique_templates(
    const std::vector<const TemplateDocument*>& templates,
    int count,
    const std::set<std::string>& excluded_ids = {}) {
    std::vector<const TemplateDocument*> chosen;
    for (const TemplateDocument* document : templates) {
        if (document == nullptr) {
            continue;
        }
        if (excluded_ids.find(document->template_id) != excluded_ids.end()) {
            continue;
        }
        chosen.push_back(document);
        if (static_cast<int>(chosen.size()) == count) {
            return chosen;
        }
    }
    if (chosen.empty()) {
        fail("could not select enough unique templates");
    }
    const size_t unique_count = chosen.size();
    while (static_cast<int>(chosen.size()) < count) {
        chosen.push_back(chosen[(chosen.size() - unique_count) % unique_count]);
    }
    return chosen;
}

TemplateCatalog build_template_catalog(const std::vector<TemplateDocument>& templates) {
    TemplateCatalog catalog;
    catalog.all.reserve(templates.size());
    for (const TemplateDocument& document : templates) {
        catalog.all.push_back(&document);
    }
    catalog.concept_a = find_first_template_from_source(templates, "A.json");
    catalog.concept_b = find_first_template_from_source(templates, "D.json");
    if (catalog.concept_a == nullptr) {
        fail("could not locate concept A template from demo_inputs/A.json");
    }
    if (catalog.concept_b == nullptr) {
        fail("could not locate concept B template from demo_inputs/D.json");
    }
    return catalog;
}

BenchmarkCase make_case(int index,
                        ScenarioKind kind,
                        const std::string& name,
                        const std::string& target,
                        int agent_count,
                        int write_count,
                        int read_count,
                        float theta,
                        int lock_hold_ms,
                        ArrivalMode arrival_mode,
                        int arrival_gap_ms,
                        const Config& config) {
    BenchmarkCase spec;
    spec.case_index = index;
    spec.kind = kind;
    spec.name = name;
    spec.target = target;
    spec.node_count = 5;
    spec.agent_count = agent_count;
    spec.write_count = write_count;
    spec.read_count = read_count;
    spec.theta = theta;
    spec.lock_hold_ms = lock_hold_ms;
    spec.arrival_mode = arrival_mode;
    spec.arrival_gap_ms = arrival_gap_ms;

    std::ostringstream collection;
    collection << config.collection_prefix
               << "_case_" << index
               << "_" << slugify(name);
    spec.collection_name = collection.str();
    return spec;
}

std::vector<BenchmarkCase> build_curated_cases(const Config& config) {
    std::vector<BenchmarkCase> cases;
    cases.reserve(10);
    cases.push_back(make_case(1,
                              ScenarioKind::kThunderingHerd,
                              "The Thundering Herd",
                              "Verify strict serialization under massive semantic overlap.",
                              10,
                              10,
                              0,
                              0.55f,
                              750,
                              ArrivalMode::kBurst,
                              0,
                              config));
    cases.push_back(make_case(2,
                              ScenarioKind::kSemanticInterleaving,
                              "The Semantic Interleaving",
                              "Verify two independent semantic hot spots can stay active in parallel.",
                              10,
                              10,
                              0,
                              0.55f,
                              750,
                              ArrivalMode::kBurst,
                              0,
                              config));
    cases.push_back(make_case(3,
                              ScenarioKind::kReadStarvationTrap,
                              "The Read-Starvation Trap",
                              "Verify read-heavy traffic still queues correctly behind conflicting writers.",
                              10,
                              2,
                              8,
                              0.55f,
                              750,
                              ArrivalMode::kStaggered,
                              40,
                              config));
    cases.push_back(make_case(4,
                              ScenarioKind::kPermissiveSieve,
                              "The Permissive Sieve",
                              "Verify a permissive theta collapses mixed semantics into a deep queue.",
                              5,
                              5,
                              0,
                              0.20f,
                              750,
                              ArrivalMode::kBurst,
                              0,
                              config));
    cases.push_back(make_case(5,
                              ScenarioKind::kStrictSieve,
                              "The Strict Sieve",
                              "Verify a strict theta preserves concurrency across mixed operations.",
                              10,
                              5,
                              5,
                              0.90f,
                              0,
                              ArrivalMode::kBurst,
                              0,
                              config));
    cases.push_back(make_case(6,
                              ScenarioKind::kGhostClient,
                              "The Ghost Client",
                              "Verify one long-held writer amplifies waiter depth without breaking ordering.",
                              5,
                              5,
                              0,
                              0.55f,
                              750,
                              ArrivalMode::kBurst,
                              0,
                              config));
    cases.push_back(make_case(7,
                              ScenarioKind::kAlmostCollision,
                              "The Almost Collision",
                              "Verify a near-threshold pair below theta stays concurrent.",
                              2,
                              2,
                              0,
                              0.55f,
                              0,
                              ArrivalMode::kBurst,
                              0,
                              config));
    cases.push_back(make_case(8,
                              ScenarioKind::kQueueHopping,
                              "Queue Hopping",
                              "Verify bursty contention produces queue hops without losing progress.",
                              20,
                              20,
                              0,
                              0.55f,
                              0,
                              ArrivalMode::kBurst,
                              0,
                              config));
    cases.push_back(make_case(9,
                              ScenarioKind::kMixedStagger,
                              "The Mixed Stagger",
                              "Verify mixed semantic pressure remains stable under long staggered arrivals.",
                              10,
                              10,
                              0,
                              0.55f,
                              750,
                              ArrivalMode::kStaggered,
                              100,
                              config));
    cases.push_back(make_case(10,
                              ScenarioKind::kReadStampede,
                              "The 100% Read Stampede",
                              "Verify fully read-only hot traffic still respects semantic exclusion.",
                              10,
                              0,
                              10,
                              0.55f,
                              0,
                              ArrivalMode::kBurst,
                              0,
                              config));
    return cases;
}

BenchmarkOperation make_operation_from_template(const TemplateDocument& document,
                                                const std::string& agent_id,
                                                OperationType operation,
                                                int64_t scheduled_offset_ms) {
    BenchmarkOperation op;
    op.agent_id = agent_id;
    op.role_prefix = role_prefix_for_source_file(document.source_file);
    op.template_id = document.template_id;
    op.text = document.text;
    op.embedding = document.embedding;
    op.operation = operation;
    op.scheduled_offset_ms = scheduled_offset_ms;
    return op;
}

BenchmarkOperation make_synthetic_operation(const std::string& template_id,
                                            const std::string& text,
                                            const std::vector<float>& embedding,
                                            const std::string& agent_id,
                                            OperationType operation,
                                            int64_t scheduled_offset_ms) {
    BenchmarkOperation op;
    op.agent_id = agent_id;
    op.role_prefix = "edge_agent";
    op.template_id = template_id;
    op.text = text;
    op.embedding = embedding;
    op.operation = operation;
    op.scheduled_offset_ms = scheduled_offset_ms;
    return op;
}

int64_t offset_for(const BenchmarkCase& spec, int ordinal) {
    if (spec.arrival_mode == ArrivalMode::kBurst) {
        return 0;
    }
    return static_cast<int64_t>(ordinal) * static_cast<int64_t>(spec.arrival_gap_ms);
}

std::string next_agent_id(std::unordered_map<std::string, int>& counters,
                          const std::string& role_prefix) {
    const int ordinal = ++counters[role_prefix];
    return role_prefix + "_" + std::to_string(ordinal);
}

std::vector<BenchmarkOperation> build_workload(const BenchmarkCase& spec,
                                               const TemplateCatalog& catalog) {
    std::vector<BenchmarkOperation> workload;
    workload.reserve(static_cast<size_t>(spec.agent_count));
    std::unordered_map<std::string, int> role_counters;

    const std::set<std::string> exclude_hot_ids = {
        catalog.concept_a->template_id,
        catalog.concept_b->template_id,
    };
    const std::vector<const TemplateDocument*> background_templates =
        pick_unique_templates(catalog.all,
                              std::max(8, spec.agent_count),
                              exclude_hot_ids);

    switch (spec.kind) {
        case ScenarioKind::kThunderingHerd: {
            for (int i = 0; i < spec.agent_count; ++i) {
                workload.push_back(
                    make_operation_from_template(*catalog.concept_a,
                                                 next_agent_id(role_counters, "sustainability_agent"),
                                                 OperationType::kWrite,
                                                 offset_for(spec, i)));
            }
            break;
        }
        case ScenarioKind::kSemanticInterleaving: {
            for (int i = 0; i < spec.agent_count; ++i) {
                const TemplateDocument& document =
                    (i % 2 == 0) ? *catalog.concept_a : *catalog.concept_b;
                workload.push_back(
                    make_operation_from_template(document,
                                                 next_agent_id(role_counters,
                                                               role_prefix_for_source_file(document.source_file)),
                                                 OperationType::kWrite,
                                                 offset_for(spec, i)));
            }
            break;
        }
        case ScenarioKind::kReadStarvationTrap: {
            for (int i = 0; i < spec.agent_count; ++i) {
                const bool is_write = (i == 0 || i == 5);
                workload.push_back(
                    make_operation_from_template(*catalog.concept_a,
                                                 next_agent_id(role_counters, "sustainability_agent"),
                                                 is_write ? OperationType::kWrite
                                                          : OperationType::kRead,
                                                 offset_for(spec, i)));
            }
            break;
        }
        case ScenarioKind::kPermissiveSieve: {
            const std::vector<const TemplateDocument*> selected = {
                catalog.concept_a,
                background_templates[0],
                catalog.concept_b,
                background_templates[1],
                background_templates[2],
            };
            for (int i = 0; i < spec.agent_count; ++i) {
                workload.push_back(
                    make_operation_from_template(*selected[static_cast<size_t>(i)],
                                                 next_agent_id(role_counters,
                                                               role_prefix_for_source_file(
                                                                   selected[static_cast<size_t>(i)]->source_file)),
                                                 OperationType::kWrite,
                                                 offset_for(spec, i)));
            }
            break;
        }
        case ScenarioKind::kStrictSieve: {
            const std::vector<const TemplateDocument*> selected =
                pick_unique_templates(catalog.all, spec.agent_count);
            for (int i = 0; i < spec.agent_count; ++i) {
                const OperationType op = (i % 2 == 0) ? OperationType::kWrite
                                                      : OperationType::kRead;
                workload.push_back(
                    make_operation_from_template(*selected[static_cast<size_t>(i)],
                                                 next_agent_id(role_counters,
                                                               role_prefix_for_source_file(
                                                                   selected[static_cast<size_t>(i)]->source_file)),
                                                 op,
                                                 offset_for(spec, i)));
            }
            break;
        }
        case ScenarioKind::kGhostClient: {
            for (int i = 0; i < spec.agent_count; ++i) {
                workload.push_back(
                    make_operation_from_template(*catalog.concept_a,
                                                 next_agent_id(role_counters, "sustainability_agent"),
                                                 OperationType::kWrite,
                                                 offset_for(spec, i)));
            }
            break;
        }
        case ScenarioKind::kAlmostCollision: {
            const float target_similarity = 0.54f;
            const float orthogonal_component =
                std::sqrt(1.0f - target_similarity * target_similarity);
            workload.push_back(
                make_synthetic_operation("synthetic-almost-left",
                                         "Synthetic benchmark payload for the near-threshold left vector.",
                                         {1.0f, 0.0f},
                                         "edge_agent_1",
                                         OperationType::kWrite,
                                         0));
            workload.push_back(
                make_synthetic_operation("synthetic-almost-right",
                                         "Synthetic benchmark payload for the near-threshold right vector.",
                                         {target_similarity, orthogonal_component},
                                         "edge_agent_2",
                                         OperationType::kWrite,
                                         0));
            break;
        }
        case ScenarioKind::kQueueHopping: {
            for (int i = 0; i < spec.agent_count; ++i) {
                workload.push_back(
                    make_operation_from_template(*catalog.concept_a,
                                                 next_agent_id(role_counters, "sustainability_agent"),
                                                 OperationType::kWrite,
                                                 offset_for(spec, i)));
            }
            break;
        }
        case ScenarioKind::kMixedStagger: {
            const std::vector<const TemplateDocument*> selected = {
                catalog.concept_a,
                background_templates[0],
                catalog.concept_b,
                background_templates[1],
                catalog.concept_a,
                background_templates[2],
                catalog.concept_b,
                background_templates[3],
                background_templates[4],
                background_templates[5],
            };
            for (int i = 0; i < spec.agent_count; ++i) {
                workload.push_back(
                    make_operation_from_template(*selected[static_cast<size_t>(i)],
                                                 next_agent_id(role_counters,
                                                               role_prefix_for_source_file(
                                                                   selected[static_cast<size_t>(i)]->source_file)),
                                                 OperationType::kWrite,
                                                 offset_for(spec, i)));
            }
            break;
        }
        case ScenarioKind::kReadStampede: {
            for (int i = 0; i < spec.agent_count; ++i) {
                workload.push_back(
                    make_operation_from_template(*catalog.concept_a,
                                                 next_agent_id(role_counters, "sustainability_agent"),
                                                 OperationType::kRead,
                                                 offset_for(spec, i)));
            }
            break;
        }
    }

    if (static_cast<int>(workload.size()) != spec.agent_count) {
        fail("scenario " + spec.name + " built the wrong number of operations");
    }

    int actual_writes = 0;
    int actual_reads = 0;
    for (const BenchmarkOperation& operation : workload) {
        if (operation.operation == OperationType::kWrite) {
            ++actual_writes;
        } else {
            ++actual_reads;
        }
    }
    if (actual_writes != spec.write_count || actual_reads != spec.read_count) {
        std::ostringstream message;
        message << "scenario " << spec.name
                << " built operation mix write/read="
                << actual_writes << "/" << actual_reads
                << " but expected " << spec.write_count << "/" << spec.read_count;
        fail(message.str());
    }
    return workload;
}

bool intervals_overlap(int64_t start_a, int64_t end_a, int64_t start_b, int64_t end_b) {
    if (start_a <= 0 || end_a <= 0 || start_b <= 0 || end_b <= 0) {
        return false;
    }
    return std::max(start_a, start_b) < std::min(end_a, end_b);
}

int64_t percentile_ms(std::vector<int64_t> values, double percentile) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const double rank = percentile * static_cast<double>(values.size() - 1);
    const size_t index = static_cast<size_t>(std::round(rank));
    return values[std::min(index, values.size() - 1)];
}

double parse_size_mib(std::string text) {
    text = trim_copy(text);
    if (text.empty()) {
        return 0.0;
    }
    size_t index = 0;
    while (index < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[index])) != 0 || text[index] == '.')) {
        ++index;
    }
    if (index == 0) {
        return 0.0;
    }
    const double value = std::stod(text.substr(0, index));
    const std::string unit = lowercase_copy(trim_copy(text.substr(index)));
    if (unit == "b") {
        return value / (1024.0 * 1024.0);
    }
    if (unit == "kb" || unit == "kib") {
        return unit == "kb" ? value / 1000.0 : value / 1024.0;
    }
    if (unit == "mb" || unit == "mib") {
        return unit == "mb" ? value * 1000.0 / 1024.0 : value;
    }
    if (unit == "gb" || unit == "gib") {
        return unit == "gb" ? value * 1000.0 : value * 1024.0;
    }
    return value;
}

std::string json_string_field(const std::string& line, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\":";
    const size_t key_pos = line.find(quoted_key);
    if (key_pos == std::string::npos) {
        return "";
    }
    const size_t first_quote = line.find('"', key_pos + quoted_key.size());
    if (first_quote == std::string::npos) {
        return "";
    }
    const size_t second_quote = line.find('"', first_quote + 1);
    if (second_quote == std::string::npos) {
        return "";
    }
    return line.substr(first_quote + 1, second_quote - first_quote - 1);
}

std::vector<ContainerStats> collect_container_stats(const std::vector<std::string>& services,
                                                    const Config& config) {
    std::vector<ContainerStats> stats;
    if (services.empty()) {
        return stats;
    }
    std::ostringstream command;
    command << "cd " << shell_quote(config.project_root) << " && ids=$(docker compose ps -q";
    for (const std::string& service : services) {
        command << " " << service;
    }
    command << "); if [ -n \"$ids\" ]; then docker stats --no-stream --format '{{json .}}' $ids; fi";
    const ShellCommandResult result = run_shell_capture(command.str());
    if (result.exit_code != 0) {
        warn("docker stats failed; skipping resource metrics");
        return stats;
    }
    std::stringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim_copy(line);
        if (line.empty()) {
            continue;
        }
        ContainerStats item;
        item.name = json_string_field(line, "Name");
        const std::string cpu = json_string_field(line, "CPUPerc");
        const std::string mem = json_string_field(line, "MemUsage");
        const std::string net = json_string_field(line, "NetIO");
        if (!cpu.empty()) {
            item.cpu_percent = std::stod(cpu.substr(0, cpu.find('%')));
        }
        if (!mem.empty()) {
            const size_t slash = mem.find('/');
            item.memory_used_mib = parse_size_mib(mem.substr(0, slash));
        }
        if (!net.empty()) {
            const size_t slash = net.find('/');
            item.net_input_mib = parse_size_mib(net.substr(0, slash));
            item.net_output_mib = parse_size_mib(slash == std::string::npos ? "" : net.substr(slash + 1));
        }
        stats.push_back(std::move(item));
    }
    return stats;
}

BenchmarkMetrics compute_metrics(const BenchmarkCase& spec,
                                 const std::vector<OperationResult>& operations) {
    BenchmarkMetrics metrics;
    metrics.total_ops = static_cast<int>(operations.size());

    std::unordered_map<std::string, OperationType> type_by_agent;
    for (const auto& result : operations) {
        type_by_agent[result.operation.agent_id] = result.operation.operation;
    }

    std::vector<int64_t> latencies;
    std::vector<int64_t> write_latencies;
    std::vector<int64_t> read_latencies;
    std::vector<int64_t> waits;
    std::vector<int64_t> qdrant_windows;
    std::vector<int64_t> wait_positions;
    std::vector<int64_t> wake_counts;
    std::vector<int64_t> queue_hops;

    int64_t first_submit = std::numeric_limits<int64_t>::max();
    int64_t last_finish = 0;

    for (const auto& result : operations) {
        if (result.operation.operation == OperationType::kWrite) {
            ++metrics.write_ops;
        } else {
            ++metrics.read_ops;
        }
        if (!result.status.ok()) {
            ++metrics.grpc_failures;
            continue;
        }
        if (result.response.granted()) {
            ++metrics.granted_ops;
        }
        if (result.response.lock_wait_ms() > 0) {
            ++metrics.blocked_ops;
            if (result.operation.operation == OperationType::kWrite) {
                ++metrics.blocked_writes;
            } else {
                ++metrics.blocked_reads;
            }
        }
        latencies.push_back(result.elapsed_ms);
        if (result.operation.operation == OperationType::kWrite) {
            write_latencies.push_back(result.elapsed_ms);
        } else {
            read_latencies.push_back(result.elapsed_ms);
        }
        waits.push_back(result.response.lock_wait_ms());
        qdrant_windows.push_back(
            std::max<int64_t>(0,
                              result.response.qdrant_write_complete_unix_ms() -
                                  result.response.lock_acquired_unix_ms()));
        wait_positions.push_back(result.response.wait_position());
        wake_counts.push_back(result.response.wake_count());
        queue_hops.push_back(result.response.queue_hops());
        metrics.active_lock_count_max =
            std::max(metrics.active_lock_count_max, result.response.active_lock_count());
        metrics.wait_position_max =
            std::max(metrics.wait_position_max, result.response.wait_position());
        metrics.wake_count_max =
            std::max(metrics.wake_count_max, result.response.wake_count());
        metrics.queue_hops_max =
            std::max(metrics.queue_hops_max, result.response.queue_hops());

        if (result.submit_ms < first_submit) {
            first_submit = result.submit_ms;
        }
        if (result.finish_ms > last_finish) {
            last_finish = result.finish_ms;
        }

        if (result.operation.operation == OperationType::kRead &&
            result.response.lock_wait_ms() > 0) {
            const auto it = type_by_agent.find(result.response.blocking_agent_id());
            if (it != type_by_agent.end() && it->second == OperationType::kWrite) {
                ++metrics.blocked_reads_on_write;
            }
        }
    }

    for (size_t i = 0; i < operations.size(); ++i) {
        for (size_t j = i + 1; j < operations.size(); ++j) {
            const float similarity =
                cosine_similarity(operations[i].operation.embedding,
                                  operations[j].operation.embedding);
            const bool overlap =
                intervals_overlap(operations[i].response.lock_acquired_unix_ms(),
                                  operations[i].response.lock_released_unix_ms(),
                                  operations[j].response.lock_acquired_unix_ms(),
                                  operations[j].response.lock_released_unix_ms());
            if (similarity >= spec.theta) {
                ++metrics.expected_conflict_pairs;
                if (overlap) {
                    ++metrics.conflicting_overlap_violations;
                }
            } else {
                ++metrics.expected_distinct_pairs;
                if (overlap) {
                    ++metrics.distinct_parallel_pairs;
                } else {
                    ++metrics.distinct_nonparallel_pairs;
                }
            }
        }
    }

    metrics.success_rate = metrics.total_ops == 0
                               ? 0.0
                               : static_cast<double>(metrics.granted_ops) /
                                     static_cast<double>(metrics.total_ops);
    metrics.makespan_ms =
        first_submit == std::numeric_limits<int64_t>::max() ? 0 : last_finish - first_submit;
    metrics.throughput_ops_per_sec =
        metrics.makespan_ms <= 0
            ? 0.0
            : (static_cast<double>(metrics.total_ops) * 1000.0) /
                  static_cast<double>(metrics.makespan_ms);
    metrics.serialization_score =
        metrics.expected_conflict_pairs == 0
            ? 1.0
            : 1.0 -
                  (static_cast<double>(metrics.conflicting_overlap_violations) /
                   static_cast<double>(metrics.expected_conflict_pairs));
    metrics.distinct_parallelism_rate =
        metrics.expected_distinct_pairs == 0
            ? 0.0
            : static_cast<double>(metrics.distinct_parallel_pairs) /
                  static_cast<double>(metrics.expected_distinct_pairs);

    metrics.latency_p50_ms = percentile_ms(latencies, 0.50);
    metrics.latency_p95_ms = percentile_ms(latencies, 0.95);
    metrics.latency_p99_ms = percentile_ms(latencies, 0.99);
    metrics.write_latency_p95_ms = percentile_ms(write_latencies, 0.95);
    metrics.read_latency_p95_ms = percentile_ms(read_latencies, 0.95);
    metrics.lock_wait_p50_ms = percentile_ms(waits, 0.50);
    metrics.lock_wait_p95_ms = percentile_ms(waits, 0.95);
    metrics.lock_wait_p99_ms = percentile_ms(waits, 0.99);
    metrics.qdrant_window_p95_ms = percentile_ms(qdrant_windows, 0.95);
    metrics.queue_position_p95 = percentile_ms(wait_positions, 0.95);
    metrics.wake_count_p95 = percentile_ms(wake_counts, 0.95);
    metrics.queue_hops_p95 = percentile_ms(queue_hops, 0.95);
    return metrics;
}

std::vector<OperationResult> run_case_traffic(const Config& config,
                                              const BenchmarkCase& spec,
                                              const std::vector<BenchmarkOperation>& workload) {
    reset_qdrant_collection(config, spec.collection_name);
    create_qdrant_collection(config, spec.collection_name, workload.front().embedding.size());

    auto channel = grpc::CreateChannel(config.dscc_target,
                                       grpc::InsecureChannelCredentials());
    const auto start = SteadyClock::now();
    const auto now_ms = [&]() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   SteadyClock::now() - start)
            .count();
    };
    const int64_t unix_base_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    std::vector<OperationResult> results(workload.size());
    std::vector<std::thread> threads;
    threads.reserve(workload.size());

    for (size_t i = 0; i < workload.size(); ++i) {
        threads.emplace_back([&, i]() {
            auto stub = dscc::LockService::NewStub(channel);
            const BenchmarkOperation& op = workload[i];
            results[i].operation = op;

            std::this_thread::sleep_until(start + std::chrono::milliseconds(op.scheduled_offset_ms));
            results[i].submit_ms = now_ms();

            dscc::AcquireRequest request;
            request.set_agent_id(op.agent_id);
            request.set_payload_text(op.text);
            request.set_source_file(op.template_id);
            request.set_timestamp_unix_ms(unix_base_ms + op.scheduled_offset_ms);
            request.set_operation_type(to_proto_operation(op.operation));
            for (float value : op.embedding) {
                request.add_embedding(value);
            }

            grpc::ClientContext context;
            results[i].dslm_enter_ms = now_ms();
            results[i].status = stub->AcquireGuard(&context, request, &results[i].response);
            results[i].dslm_exit_ms = now_ms();
            results[i].finish_ms = results[i].dslm_exit_ms;
            results[i].elapsed_ms = results[i].finish_ms - results[i].submit_ms;
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const int expected_writes = static_cast<int>(
        std::count_if(workload.begin(),
                      workload.end(),
                      [](const BenchmarkOperation& op) {
                          return op.operation == OperationType::kWrite;
                      }));
    const int actual_points = qdrant_point_count(config, spec.collection_name);
    if (actual_points != expected_writes) {
        std::ostringstream oss;
        oss << "Qdrant point count mismatch for case " << spec.case_index
            << ": expected=" << expected_writes
            << " actual=" << actual_points;
        fail(oss.str());
    }
    return results;
}

void set_case_environment(const Config& config,
                          const BenchmarkCase& spec) {
    ::setenv("EMBEDDING_IMAGE", config.embedding_image.c_str(), 1);
    ::setenv("EMBEDDING_MODEL_ID", config.model_id.c_str(), 1);
    ::setenv("INFINITY_IMAGE", config.embedding_image.c_str(), 1);
    ::setenv("INFINITY_MODEL_ID", config.model_id.c_str(), 1);
    ::setenv("QDRANT_COLLECTION", spec.collection_name.c_str(), 1);
    {
        std::ostringstream theta;
        theta << std::fixed << std::setprecision(2) << spec.theta;
        ::setenv("DSCC_THETA", theta.str().c_str(), 1);
    }
    {
        const std::string hold = std::to_string(spec.lock_hold_ms);
        ::setenv("DSCC_LOCK_HOLD_MS", hold.c_str(), 1);
    }
}

void start_stack_for_case(const Config& config, const BenchmarkCase& spec) {
    set_case_environment(config, spec);
    run_shell_or_throw(
        compose_command(
            config,
            "up -d --build --force-recreate qdrant embedding-service "
            "dscc-node-1 dscc-node-2 dscc-node-3 dscc-node-4 dscc-node-5 dscc-proxy"),
        "docker compose up failed");
    wait_for_http_service("Qdrant", config.qdrant_host, config.qdrant_port, {"/collections", "/"});
    wait_for_embedding_service(config);
    ensure_ollama_model_available(config);
    wait_for_leader(config.node_targets, config, std::chrono::seconds(15));
    wait_for_dscc_service(config);
}

void stop_stack(const Config& config) {
    const ShellCommandResult result = run_shell_capture(compose_command(config, "down"));
    if (result.exit_code != 0) {
        warn("docker compose down returned non-zero");
    }
}

std::string default_output_path(const Config& config) {
    const auto now = std::chrono::system_clock::now();
    const auto unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  now.time_since_epoch())
                                  .count();
    return (fs::path(config.project_root) / "logs" /
            ("benchmark_run_" + std::to_string(unix_seconds) + ".json"))
        .string();
}

std::string grpc_status_code_name(grpc::StatusCode code) {
    switch (code) {
        case grpc::StatusCode::OK:
            return "OK";
        case grpc::StatusCode::CANCELLED:
            return "CANCELLED";
        case grpc::StatusCode::UNKNOWN:
            return "UNKNOWN";
        case grpc::StatusCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case grpc::StatusCode::DEADLINE_EXCEEDED:
            return "DEADLINE_EXCEEDED";
        case grpc::StatusCode::NOT_FOUND:
            return "NOT_FOUND";
        case grpc::StatusCode::ALREADY_EXISTS:
            return "ALREADY_EXISTS";
        case grpc::StatusCode::PERMISSION_DENIED:
            return "PERMISSION_DENIED";
        case grpc::StatusCode::RESOURCE_EXHAUSTED:
            return "RESOURCE_EXHAUSTED";
        case grpc::StatusCode::FAILED_PRECONDITION:
            return "FAILED_PRECONDITION";
        case grpc::StatusCode::ABORTED:
            return "ABORTED";
        case grpc::StatusCode::OUT_OF_RANGE:
            return "OUT_OF_RANGE";
        case grpc::StatusCode::UNIMPLEMENTED:
            return "UNIMPLEMENTED";
        case grpc::StatusCode::INTERNAL:
            return "INTERNAL";
        case grpc::StatusCode::UNAVAILABLE:
            return "UNAVAILABLE";
        case grpc::StatusCode::DATA_LOSS:
            return "DATA_LOSS";
        case grpc::StatusCode::UNAUTHENTICATED:
            return "UNAUTHENTICATED";
    }
    return "UNKNOWN";
}

void write_template_catalog_json(std::ostream& out,
                                 const std::vector<TemplateDocument>& templates) {
    out << "  \"templates\": [\n";
    for (size_t i = 0; i < templates.size(); ++i) {
        const TemplateDocument& item = templates[i];
        out << "    {\"template_id\":\"" << escape_json(item.template_id)
            << "\",\"source_file\":\"" << escape_json(item.source_file)
            << "\",\"payload_text\":\"" << escape_json(item.text) << "\"}";
        out << (i + 1 == templates.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
}

void write_case_json(std::ostream& out,
                     const BenchmarkCaseResult& result) {
    const std::vector<ViolationRecord> violations =
        compute_violation_records(result.spec, result.operations);
    out << "    {\n";
    out << "      \"case_index\": " << result.spec.case_index << ",\n";
    out << "      \"name\": \"" << escape_json(result.spec.name) << "\",\n";
    out << "      \"target\": \"" << escape_json(result.spec.target) << "\",\n";
    out << "      \"node_count\": " << result.spec.node_count << ",\n";
    out << "      \"agent_count\": " << result.spec.agent_count << ",\n";
    out << "      \"write_count\": " << result.spec.write_count << ",\n";
    out << "      \"read_count\": " << result.spec.read_count << ",\n";
    out << "      \"theta\": " << std::fixed << std::setprecision(3) << result.spec.theta << ",\n";
    out << "      \"lock_hold_ms\": " << result.spec.lock_hold_ms << ",\n";
    out << "      \"arrival_mode\": \""
        << (result.spec.arrival_mode == ArrivalMode::kBurst ? "burst" : "staggered")
        << "\",\n";
    out << "      \"arrival_gap_ms\": " << result.spec.arrival_gap_ms << ",\n";
    out << "      \"collection_name\": \"" << escape_json(result.spec.collection_name) << "\",\n";
    out << "      \"metrics\": {\n";
    const BenchmarkMetrics& m = result.metrics;
    out << "        \"total_ops\": " << m.total_ops << ",\n";
    out << "        \"write_ops\": " << m.write_ops << ",\n";
    out << "        \"read_ops\": " << m.read_ops << ",\n";
    out << "        \"grpc_failures\": " << m.grpc_failures << ",\n";
    out << "        \"granted_ops\": " << m.granted_ops << ",\n";
    out << "        \"blocked_ops\": " << m.blocked_ops << ",\n";
    out << "        \"blocked_writes\": " << m.blocked_writes << ",\n";
    out << "        \"blocked_reads\": " << m.blocked_reads << ",\n";
    out << "        \"blocked_reads_on_write\": " << m.blocked_reads_on_write << ",\n";
    out << "        \"expected_conflict_pairs\": " << m.expected_conflict_pairs << ",\n";
    out << "        \"expected_distinct_pairs\": " << m.expected_distinct_pairs << ",\n";
    out << "        \"conflicting_overlap_violations\": " << m.conflicting_overlap_violations << ",\n";
    out << "        \"distinct_parallel_pairs\": " << m.distinct_parallel_pairs << ",\n";
    out << "        \"distinct_nonparallel_pairs\": " << m.distinct_nonparallel_pairs << ",\n";
    out << "        \"active_lock_count_max\": " << m.active_lock_count_max << ",\n";
    out << "        \"wait_position_max\": " << m.wait_position_max << ",\n";
    out << "        \"wake_count_max\": " << m.wake_count_max << ",\n";
    out << "        \"queue_hops_max\": " << m.queue_hops_max << ",\n";
    out << "        \"success_rate\": " << std::fixed << std::setprecision(6) << m.success_rate << ",\n";
    out << "        \"throughput_ops_per_sec\": " << m.throughput_ops_per_sec << ",\n";
    out << "        \"serialization_score\": " << m.serialization_score << ",\n";
    out << "        \"distinct_parallelism_rate\": " << m.distinct_parallelism_rate << ",\n";
    out << "        \"makespan_ms\": " << m.makespan_ms << ",\n";
    out << "        \"latency_p50_ms\": " << m.latency_p50_ms << ",\n";
    out << "        \"latency_p95_ms\": " << m.latency_p95_ms << ",\n";
    out << "        \"latency_p99_ms\": " << m.latency_p99_ms << ",\n";
    out << "        \"write_latency_p95_ms\": " << m.write_latency_p95_ms << ",\n";
    out << "        \"read_latency_p95_ms\": " << m.read_latency_p95_ms << ",\n";
    out << "        \"lock_wait_p50_ms\": " << m.lock_wait_p50_ms << ",\n";
    out << "        \"lock_wait_p95_ms\": " << m.lock_wait_p95_ms << ",\n";
    out << "        \"lock_wait_p99_ms\": " << m.lock_wait_p99_ms << ",\n";
    out << "        \"qdrant_window_p95_ms\": " << m.qdrant_window_p95_ms << ",\n";
    out << "        \"queue_position_p95\": " << m.queue_position_p95 << ",\n";
    out << "        \"wake_count_p95\": " << m.wake_count_p95 << ",\n";
    out << "        \"queue_hops_p95\": " << m.queue_hops_p95 << "\n";
    out << "      },\n";
    out << "      \"container_stats\": [\n";
    for (size_t j = 0; j < result.container_stats.size(); ++j) {
        const ContainerStats& stats = result.container_stats[j];
        out << "        {\"name\":\"" << escape_json(stats.name)
            << "\",\"cpu_percent\":" << std::fixed << std::setprecision(3) << stats.cpu_percent
            << ",\"memory_used_mib\":" << stats.memory_used_mib
            << ",\"net_input_mib\":" << stats.net_input_mib
            << ",\"net_output_mib\":" << stats.net_output_mib << "}";
        out << (j + 1 == result.container_stats.size() ? "\n" : ",\n");
    }
    out << "      ],\n";
    out << "      \"violations\": [\n";
    for (size_t j = 0; j < violations.size(); ++j) {
        const ViolationRecord& violation = violations[j];
        out << "        {\"active_agent_id\":\"" << escape_json(violation.active_agent_id)
            << "\",\"violating_agent_id\":\"" << escape_json(violation.violating_agent_id)
            << "\",\"active_operation\":\"" << operation_name(violation.active_operation)
            << "\",\"violating_operation\":\"" << operation_name(violation.violating_operation)
            << "\",\"similarity\":" << std::fixed << std::setprecision(6) << violation.similarity
            << ",\"detected_unix_ms\":" << violation.detected_unix_ms
            << ",\"overlap_start_unix_ms\":" << violation.overlap_start_unix_ms
            << ",\"overlap_end_unix_ms\":" << violation.overlap_end_unix_ms << "}";
        out << (j + 1 == violations.size() ? "\n" : ",\n");
    }
    out << "      ],\n";
    out << "      \"operations\": [\n";
    for (size_t j = 0; j < result.operations.size(); ++j) {
        const OperationResult& op = result.operations[j];
        out << "        {\n";
        out << "          \"agent_id\": \"" << escape_json(op.operation.agent_id) << "\",\n";
        out << "          \"template_id\": \"" << escape_json(op.operation.template_id) << "\",\n";
        out << "          \"payload_text\": \"" << escape_json(op.operation.text) << "\",\n";
        out << "          \"operation\": \"" << operation_name(op.operation.operation) << "\",\n";
        out << "          \"scheduled_offset_ms\": " << op.operation.scheduled_offset_ms << ",\n";
        out << "          \"submit_ms\": " << op.submit_ms << ",\n";
        out << "          \"dslm_enter_ms\": " << op.dslm_enter_ms << ",\n";
        out << "          \"dslm_exit_ms\": " << op.dslm_exit_ms << ",\n";
        out << "          \"finish_ms\": " << op.finish_ms << ",\n";
        out << "          \"elapsed_ms\": " << op.elapsed_ms << ",\n";
        out << "          \"status_ok\": " << (op.status.ok() ? "true" : "false") << ",\n";
        out << "          \"status_code\": \""
            << grpc_status_code_name(op.status.error_code()) << "\",\n";
        out << "          \"status_message\": \"" << escape_json(op.status.error_message()) << "\",\n";
        out << "          \"response_message\": \"" << escape_json(op.response.message()) << "\",\n";
        out << "          \"granted\": " << (op.response.granted() ? "true" : "false") << ",\n";
        out << "          \"server_received_unix_ms\": " << op.response.server_received_unix_ms() << ",\n";
        out << "          \"lock_acquired_unix_ms\": " << op.response.lock_acquired_unix_ms() << ",\n";
        out << "          \"qdrant_write_complete_unix_ms\": " << op.response.qdrant_write_complete_unix_ms() << ",\n";
        out << "          \"lock_released_unix_ms\": " << op.response.lock_released_unix_ms() << ",\n";
        out << "          \"lock_wait_ms\": " << op.response.lock_wait_ms() << ",\n";
        out << "          \"wait_position\": " << op.response.wait_position() << ",\n";
        out << "          \"wake_count\": " << op.response.wake_count() << ",\n";
        out << "          \"queue_hops\": " << op.response.queue_hops() << ",\n";
        out << "          \"active_lock_count\": " << op.response.active_lock_count() << ",\n";
        out << "          \"blocking_agent_id\": \"" << escape_json(op.response.blocking_agent_id()) << "\",\n";
        out << "          \"blocking_similarity_score\": " << std::fixed << std::setprecision(6)
            << op.response.blocking_similarity_score() << "\n";
        out << "        }" << (j + 1 == result.operations.size() ? "\n" : ",\n");
    }
    out << "      ]\n";
    out << "    }";
}

std::string short_blocking_agent_id(const std::string& agent_id) {
    return agent_id;
}

std::string format_zero_padded_ms(int64_t value) {
    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << value;
    return out.str();
}

std::string format_float(double value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::vector<ViolationRecord> compute_violation_records(const BenchmarkCase& spec,
                                                       const std::vector<OperationResult>& operations) {
    std::vector<ViolationRecord> violations;
    for (size_t i = 0; i < operations.size(); ++i) {
        for (size_t j = i + 1; j < operations.size(); ++j) {
            const OperationResult& left = operations[i];
            const OperationResult& right = operations[j];
            if (!left.status.ok() || !right.status.ok() ||
                !left.response.granted() || !right.response.granted()) {
                continue;
            }

            const float similarity =
                cosine_similarity(left.operation.embedding, right.operation.embedding);
            if (similarity < spec.theta) {
                continue;
            }

            const int64_t left_start = left.response.lock_acquired_unix_ms();
            const int64_t left_end = left.response.lock_released_unix_ms();
            const int64_t right_start = right.response.lock_acquired_unix_ms();
            const int64_t right_end = right.response.lock_released_unix_ms();
            if (!intervals_overlap(left_start, left_end, right_start, right_end)) {
                continue;
            }

            // Attribute the violation to the later conflicting grant: by the
            // time the second lock becomes active, the overlap bug is already
            // observable on the timeline.
            const OperationResult* active = &left;
            const OperationResult* violating = &right;
            if (right_start < left_start) {
                active = &right;
                violating = &left;
            }

            ViolationRecord record;
            record.active_agent_id = active->operation.agent_id;
            record.violating_agent_id = violating->operation.agent_id;
            record.active_operation = active->operation.operation;
            record.violating_operation = violating->operation.operation;
            record.similarity = similarity;
            record.detected_unix_ms =
                std::max(active->response.lock_acquired_unix_ms(),
                         violating->response.lock_acquired_unix_ms());
            record.overlap_start_unix_ms =
                std::max(left.response.lock_acquired_unix_ms(),
                         right.response.lock_acquired_unix_ms());
            record.overlap_end_unix_ms =
                std::min(left.response.lock_released_unix_ms(),
                         right.response.lock_released_unix_ms());
            violations.push_back(std::move(record));
        }
    }
    return violations;
}

std::vector<TimelineLine> build_timeline_lines(const BenchmarkCaseResult& result) {
    std::vector<TimelineLine> lines;
    size_t sequence = 0;
    auto push_line = [&](int64_t absolute_ms, const char* color, std::string text) {
        if (absolute_ms <= 0) {
            return;
        }
        lines.push_back(TimelineLine{absolute_ms, sequence++, color, std::move(text)});
    };

    for (const OperationResult& op : result.operations) {
        if (!op.status.ok() || !op.response.granted()) {
            std::ostringstream line;
            line << "🔴 [" << operation_title(op.operation.operation) << "] "
                 << op.operation.agent_id << " ➔ FAILED";
            if (!op.status.ok()) {
                line << " (" << grpc_status_code_name(op.status.error_code()) << ")";
            } else if (!op.response.message().empty()) {
                line << " (" << op.response.message() << ")";
            }
            const int64_t failed_at =
                op.response.server_received_unix_ms() > 0 ? op.response.server_received_unix_ms()
                                                          : std::max<int64_t>(1, op.submit_ms);
            push_line(failed_at, ansi::kRed, line.str());
            continue;
        }

        if (op.response.lock_wait_ms() > 0) {
            std::ostringstream blocked;
            blocked << "🟡 [" << operation_title(op.operation.operation) << "] "
                    << op.operation.agent_id << " ➔ BLOCKED";
            blocked << " (Sim: "
                    << format_float(op.response.blocking_similarity_score(), 2)
                    << " ➔ Waiting on "
                    << short_blocking_agent_id(op.response.blocking_agent_id()) << ")";
            push_line(op.response.server_received_unix_ms(), ansi::kYellow, blocked.str());
        }

        {
            std::ostringstream granted;
            granted << "🟢 [" << operation_title(op.operation.operation) << "] "
                    << op.operation.agent_id << " ➔ GRANTED";
            if (op.response.lock_wait_ms() > 0) {
                granted << " (Wait: " << op.response.lock_wait_ms()
                        << "ms, Hops: " << op.response.queue_hops() << ")";
            } else {
                granted << " (Active Locks: " << op.response.active_lock_count() << ")";
            }
            push_line(op.response.lock_acquired_unix_ms(), ansi::kGreen, granted.str());
        }

        {
            std::ostringstream released;
            released << "⚪ [" << operation_title(op.operation.operation) << "] "
                     << op.operation.agent_id << " ➔ RELEASED";
            push_line(op.response.lock_released_unix_ms(), ansi::kGray, released.str());
        }
    }

    const std::vector<ViolationRecord> violations =
        compute_violation_records(result.spec, result.operations);
    for (const ViolationRecord& violation : violations) {
        std::ostringstream line;
        line << "❌ [Violation] "
             << violation.violating_agent_id
             << " was granted while "
             << violation.active_agent_id
             << " was still active";
        line << " (Sim: " << format_float(violation.similarity, 2)
             << " >= Theta: " << format_float(result.spec.theta, 2) << ")";
        push_line(violation.detected_unix_ms, ansi::kRed, line.str());
    }

    std::sort(lines.begin(), lines.end(), [](const TimelineLine& left, const TimelineLine& right) {
        if (left.absolute_ms != right.absolute_ms) {
            return left.absolute_ms < right.absolute_ms;
        }
        return left.sequence < right.sequence;
    });
    return lines;
}

void print_case_terminal_report(const BenchmarkCaseResult& result,
                                const std::string& raw_output_display_name) {
    static constexpr const char* kDivider =
        "================================================================================";

    const std::vector<TimelineLine> timeline = build_timeline_lines(result);
    const int64_t base_time = timeline.empty() ? 0 : timeline.front().absolute_ms;
    const int read_percentage =
        result.spec.agent_count == 0
            ? 0
            : static_cast<int>(std::lround(
                  (100.0 * static_cast<double>(result.spec.read_count)) /
                  static_cast<double>(result.spec.agent_count)));
    const int write_percentage =
        result.spec.agent_count == 0 ? 0 : 100 - read_percentage;

    const bool correctness_ok = result.metrics.conflicting_overlap_violations == 0;
    const std::string correctness_icon = correctness_ok ? "✅" : "❌";
    const std::string serialization_percent =
        std::to_string(static_cast<int>(std::lround(result.metrics.serialization_score * 100.0)));

    std::cout << kDivider << "\n";
    std::cout << ansi::kBold << ansi::kCyan
              << "🧪 TEST CASE " << result.spec.case_index << ": " << result.spec.name
              << ansi::kReset << "\n";
    std::cout << kDivider << "\n";
    std::cout << "Target: " << result.spec.target << "\n";
    std::cout << "Params: Nodes: " << result.spec.node_count
              << " | Agents: " << result.spec.agent_count
              << " | R/W: " << read_percentage << "/" << write_percentage
              << " | Theta: " << format_float(result.spec.theta, 2)
              << " | Arrival: " << arrival_mode_label(result.spec) << "\n\n";

    std::cout << ansi::kBold << ansi::kBlue << "[ TIMELINE ]" << ansi::kReset << "\n";
    for (const TimelineLine& line : timeline) {
        const int64_t relative_ms = line.absolute_ms - base_time;
        std::cout << line.color
                  << "[" << format_zero_padded_ms(relative_ms) << "ms] "
                  << line.text
                  << ansi::kReset << "\n";
    }
    if (timeline.empty()) {
        std::cout << ansi::kRed << "[0000ms] 🔴 No timeline events captured" << ansi::kReset << "\n";
    }
    std::cout << "\n";

    std::cout << ansi::kBold << ansi::kMagenta << "[ RESULTS ]" << ansi::kReset << "\n";
    std::cout << correctness_icon << " Correctness:    "
              << result.metrics.conflicting_overlap_violations
              << " Violations (" << serialization_percent << "% Serialization)\n";
    std::cout << "✅ Parallelism:    "
              << result.metrics.distinct_parallel_pairs
              << " distinct pairs / " << result.metrics.expected_distinct_pairs
              << " allowed\n";
    std::cout << "⏱️ Latency (P95): " << result.metrics.latency_p95_ms << "ms\n";
    std::cout << "📊 Throughput:    "
              << format_float(result.metrics.throughput_ops_per_sec, 2)
              << " ops/sec\n";
    std::cout << "⚠️ Max Waiters:    "
              << result.metrics.wait_position_max
              << " (Max Hops: " << result.metrics.queue_hops_max << ")\n";
    std::cout << "💾 Raw data saved to: " << raw_output_display_name << "\n";
    std::cout << kDivider << "\n";
}

class LiveQueueMonitor {
public:
    explicit LiveQueueMonitor(const Config& config)
        : config_(config) {}

    ~LiveQueueMonitor() {
        stop();
    }

    void start(const BenchmarkCase& spec) {
        stop();
        seen_lines_.clear();
        running_.store(true);
        std::cout << ansi::kBold << ansi::kBlue << "[ LIVE QUEUE EVENTS ]" << ansi::kReset << "\n";
        std::cout << ansi::kGray
                  << "Watching queue decisions for " << spec.name << "..."
                  << ansi::kReset << "\n";
        worker_ = std::thread([this]() { run(); });
    }

    void stop() {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run() {
        while (running_.load()) {
            poll_once();
            for (int i = 0; i < 10 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        poll_once();
    }

    void poll_once() {
        std::ostringstream command;
        command << "cd " << shell_quote(config_.project_root)
                << " && docker compose logs --no-color --tail=400";
        for (const std::string& service : config_.node_service_names) {
            command << " " << service;
        }
        const ShellCommandResult result = run_shell_capture(command.str());
        if (result.exit_code != 0) {
            return;
        }

        std::stringstream lines(result.output);
        std::string line;
        while (std::getline(lines, line)) {
            const size_t tag_pos = line.find("[LOCK_");
            if (tag_pos == std::string::npos) {
                continue;
            }
            const std::string event = trim_copy(line.substr(tag_pos));
            // Docker logs are polled rather than streamed so the benchmark can
            // stay single-process; de-dup locally to keep the live view stable.
            if (!seen_lines_.insert(event).second) {
                continue;
            }
            print_event(event);
        }
    }

    void print_event(const std::string& event) {
        const char* color = ansi::kYellow;
        if (event.find("[LOCK_GRANT]") == 0) {
            color = ansi::kGreen;
        } else if (event.find("[LOCK_REQUEUE]") == 0) {
            color = ansi::kMagenta;
        }
        std::cout << color << "  " << event << ansi::kReset << "\n";
    }

    Config config_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::set<std::string> seen_lines_;
};

class BenchmarkRunLogger {
public:
    BenchmarkRunLogger(const std::string& path,
                       const Config& config,
                       const std::vector<TemplateDocument>& templates,
                       size_t planned_cases)
        : path_(path) {
        const fs::path file_path(path_);
        if (!file_path.parent_path().empty()) {
            fs::create_directories(file_path.parent_path());
        }

        out_.open(path_, std::ios::out | std::ios::trunc);
        if (!out_) {
            fail("could not open benchmark output path " + path_);
        }

        const auto now = std::chrono::system_clock::now();
        const auto unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                      now.time_since_epoch())
                                      .count();

        out_ << "{\n";
        out_ << "  \"run_started_unix_s\": " << unix_seconds << ",\n";
        out_ << "  \"project_root\": \"" << escape_json(config.project_root) << "\",\n";
        out_ << "  \"model_id\": \"" << escape_json(config.model_id) << "\",\n";
        out_ << "  \"planned_case_count\": " << planned_cases << ",\n";
        out_ << "  \"template_count\": " << templates.size() << ",\n";
        write_template_catalog_json(out_, templates);
        out_ << "  \"cases\": [\n";
        out_.flush();
    }

    ~BenchmarkRunLogger() {
        try {
            finalize();
        } catch (const std::exception&) {
        }
    }

    void append_case(const BenchmarkCaseResult& result) {
        if (finalized_) {
            fail("attempted to append to finalized benchmark log");
        }
        if (!first_case_) {
            out_ << ",\n";
        }
        write_case_json(out_, result);
        out_.flush();
        first_case_ = false;
        print_case_terminal_report(result, fs::path(path_).filename().string());
    }

    void finalize() {
        if (finalized_) {
            return;
        }
        out_ << "\n  ]\n";
        out_ << "}\n";
        out_.flush();
        finalized_ = true;
    }

    const std::string& path() const {
        return path_;
    }

private:
    std::string path_;
    std::ofstream out_;
    bool first_case_ = true;
    bool finalized_ = false;
};

}  // namespace

int main() {
    Config config;
    bool stack_started = false;
    try {
        config = load_config();
        const std::vector<BenchmarkCase> cases = build_curated_cases(config);
        if (cases.empty()) {
            fail("curated benchmark list was empty");
        }

        info("curated benchmark cases: " + std::to_string(cases.size()));
        start_stack_for_case(config, cases.front());
        stack_started = true;

        const std::vector<TemplateDocument> templates = load_templates(config);
        const TemplateCatalog catalog = build_template_catalog(templates);
        info("loaded template corpus size: " + std::to_string(templates.size()));

        stop_stack(config);
        stack_started = false;

        const std::string output_path =
            config.output_path.empty() ? default_output_path(config) : config.output_path;
        BenchmarkRunLogger logger(output_path, config, templates, cases.size());
        LiveQueueMonitor queue_monitor(config);

        for (const BenchmarkCase& spec : cases) {
            start_stack_for_case(config, spec);
            stack_started = true;

            const std::vector<BenchmarkOperation> workload = build_workload(spec, catalog);
            queue_monitor.start(spec);
            const std::vector<OperationResult> operations = run_case_traffic(config, spec, workload);
            queue_monitor.stop();
            std::cout << "\n";

            std::vector<std::string> active_services = {"qdrant", "embedding-service", "dscc-proxy"};
            for (int i = 0; i < spec.node_count; ++i) {
                active_services.push_back(config.node_service_names[static_cast<size_t>(i)]);
            }

            BenchmarkCaseResult result;
            result.spec = spec;
            result.operations = operations;
            result.metrics = compute_metrics(spec, operations);
            result.container_stats = collect_container_stats(active_services, config);
            logger.append_case(result);

            if (config.teardown_on_exit) {
                stop_stack(config);
                stack_started = false;
            }
        }

        logger.finalize();
        info("raw benchmark log written to " + logger.path());

        if (config.teardown_on_exit) {
            stop_stack(config);
        }
        return 0;
    } catch (const std::exception& ex) {
        if (stack_started && config.teardown_on_exit) {
            stop_stack(config);
        }
        std::cerr << "[ERROR] " << ex.what() << std::endl;
        return 1;
    }
}
