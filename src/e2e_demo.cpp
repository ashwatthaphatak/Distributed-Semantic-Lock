// Choreographed demo for a ~5-minute distributed systems presentation.
// Drives continuous traffic through the full Docker stack (embedding -> proxy -> Raft nodes -> Qdrant)
// while executing three scripted chaos scenarios:
//   1) Leader failover
//   2) Rejoined node catch-up
//   3) Quorum collapse (kill 3 followers; system halts because quorum cannot be reached)
// The demo ends immediately after the quorum-collapse scenario.
//
// Build target: dscc-e2e-demo
// Usage:       ./build/dscc-e2e-demo          (uses defaults, ~5 minutes)
//
// Environment overrides:
//   DSCC_THETA, DSCC_LOCK_HOLD_MS, QDRANT_COLLECTION,
//   EMBEDDING_IMAGE, EMBEDDING_MODEL_ID,
//   E2E_TEARDOWN (1 to tear down on exit),
//   DEMO_OP_INTERVAL_MS (ms between operations per agent, default 1000),
//   DEMO_DURATION_SEC   (total demo runtime in seconds, default 300 = 5 min)

#include "dscc.grpc.pb.h"
#include "dscc_raft.grpc.pb.h"
#include "threadsafe_log.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;
using SteadyTime  = SteadyClock::time_point;

// ---------------------------------------------------------------------------
// ANSI colors
// ---------------------------------------------------------------------------

namespace ansi {
constexpr const char* kReset   = "\033[0m";
constexpr const char* kBold    = "\033[1m";
constexpr const char* kDim     = "\033[2m";
constexpr const char* kBlue    = "\033[34m";
constexpr const char* kGreen   = "\033[32m";
constexpr const char* kYellow  = "\033[33m";
constexpr const char* kRed     = "\033[31m";
constexpr const char* kMagenta = "\033[35m";
constexpr const char* kCyan    = "\033[36m";
constexpr const char* kWhite   = "\033[37m";
constexpr const char* kBgRed   = "\033[41m";
constexpr const char* kBgGreen = "\033[42m";
constexpr const char* kBgBlue  = "\033[44m";
}  // namespace ansi

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    std::string project_root    = DSLM_PROJECT_ROOT;
    std::string input_dir       = DSLM_PROJECT_ROOT "/demo_inputs";
    std::string embedding_image = "ollama/ollama:latest";
    std::string model_id        = "all-minilm:latest";
    std::string collection      = "dscc_memory_demo";
    std::string embedding_host  = "127.0.0.1";
    std::string embedding_port  = "7997";
    std::string qdrant_host     = "127.0.0.1";
    std::string qdrant_port     = "6333";
    std::string dscc_target     = "127.0.0.1:50050";

    std::vector<std::string> node_targets = {
        "127.0.0.1:50051", "127.0.0.1:50052", "127.0.0.1:50053",
        "127.0.0.1:50054", "127.0.0.1:50055",
    };
    std::vector<std::string> node_service_names = {
        "dscc-node-1", "dscc-node-2", "dscc-node-3",
        "dscc-node-4", "dscc-node-5",
    };

    float theta        = 0.78f;
    int   lock_hold_ms = 500;
    bool  teardown_on_exit = false;

    int op_interval_ms  = 1000;
    int duration_sec    = 300;
};

// ---------------------------------------------------------------------------
// Lightweight HTTP (same raw-socket approach as e2e_bench)
// ---------------------------------------------------------------------------

struct HttpResponse {
    int         status_code = 0;
    std::string body;
};

bool send_all(int socket_fd, const std::string& payload) {
    size_t total_sent = 0;
    while (total_sent < payload.size()) {
        const ssize_t sent =
            ::send(socket_fd, payload.data() + total_sent,
                   payload.size() - total_sent, 0);
        if (sent <= 0) return false;
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
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addresses = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0)
        throw std::runtime_error("DNS resolution failed for " + host + ":" + port);

    int socket_fd = -1;
    for (auto* addr = addresses; addr; addr = addr->ai_next) {
        socket_fd = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (socket_fd < 0) continue;
        if (::connect(socket_fd, addr->ai_addr, addr->ai_addrlen) == 0) break;
        ::close(socket_fd);
        socket_fd = -1;
    }
    ::freeaddrinfo(addresses);
    if (socket_fd < 0)
        throw std::runtime_error("connect failed for " + host + ":" + port);

    std::ostringstream req;
    req << method << " " << target << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n";
    if (!body.empty()) {
        req << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n" << body;

    if (!send_all(socket_fd, req.str())) {
        ::close(socket_fd);
        throw std::runtime_error("send failed");
    }

    std::string raw;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(socket_fd, buf, sizeof(buf), 0);
        if (n < 0) { ::close(socket_fd); throw std::runtime_error("recv failed"); }
        if (n == 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }
    ::close(socket_fd);

    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos)
        throw std::runtime_error("malformed HTTP response");

    std::istringstream hdr(raw.substr(0, hdr_end));
    std::string version;
    int code = 0;
    hdr >> version >> code;
    return {code, raw.substr(hdr_end + 4)};
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers (same approach as e2e_bench; avoids adding deps)
// ---------------------------------------------------------------------------

std::string escape_json(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o.push_back(c);
        }
    }
    return o;
}

std::string parse_json_string(const std::string& body, const std::string& key) {
    auto qk = "\"" + key + "\"";
    auto kp = body.find(qk);
    if (kp == std::string::npos) throw std::runtime_error("missing field: " + key);
    auto vs = body.find('"', body.find(':', kp) + 1);
    if (vs == std::string::npos) throw std::runtime_error("bad field: " + key);
    ++vs;
    std::string val;
    bool esc = false;
    for (size_t i = vs; i < body.size(); ++i) {
        char ch = body[i];
        if (esc) { val.push_back(ch); esc = false; continue; }
        if (ch == '\\') { esc = true; continue; }
        if (ch == '"') return val;
        val.push_back(ch);
    }
    throw std::runtime_error("unterminated string: " + key);
}

std::string parse_json_string_opt(const std::string& body, const std::string& key,
                                  const std::string& def) {
    if (body.find("\"" + key + "\"") == std::string::npos) return def;
    return parse_json_string(body, key);
}

int64_t parse_json_int(const std::string& body, const std::string& key, int64_t def) {
    auto qk = "\"" + key + "\"";
    auto kp = body.find(qk);
    if (kp == std::string::npos) return def;
    auto colon = body.find(':', kp);
    if (colon == std::string::npos) return def;
    size_t s = colon + 1;
    while (s < body.size() && std::isspace(static_cast<unsigned char>(body[s]))) ++s;
    size_t e = s;
    if (e < body.size() && (body[e] == '-' || body[e] == '+')) ++e;
    while (e < body.size() && std::isdigit(static_cast<unsigned char>(body[e]))) ++e;
    return std::stoll(body.substr(s, e - s));
}

std::optional<std::string> parse_first_schedule_entry(const std::string& body) {
    auto kp = body.find("\"payload_schedule\"");
    if (kp == std::string::npos) return std::nullopt;
    auto arr = body.find('[', kp);
    if (arr == std::string::npos) return std::nullopt;

    bool in_str = false, esc = false;
    int depth = 0;
    size_t obj_start = std::string::npos;
    for (size_t i = arr + 1; i < body.size(); ++i) {
        char ch = body[i];
        if (esc) { esc = false; continue; }
        if (ch == '\\') { esc = true; continue; }
        if (ch == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (ch == '{') { if (depth == 0) obj_start = i; ++depth; }
        else if (ch == '}') {
            if (depth > 0 && --depth == 0 && obj_start != std::string::npos)
                return body.substr(obj_start, i - obj_start + 1);
        }
        else if (ch == ']' && depth == 0) break;
    }
    return std::nullopt;
}

float parse_float_token(const std::string& tok) {
    char* end = nullptr;
    float v = std::strtof(tok.c_str(), &end);
    if (end == tok.c_str()) throw std::runtime_error("bad float: " + tok);
    return v;
}

std::vector<float> parse_embedding_array(const std::string& body) {
    auto kp = body.find("\"embedding\"");
    if (kp == std::string::npos)
        throw std::runtime_error("no embedding field in response");
    auto a = body.find('[', kp);
    if (a == std::string::npos) throw std::runtime_error("malformed embedding");
    size_t pos = a + 1;
    int depth = 1;
    size_t ae = std::string::npos;
    while (pos < body.size()) {
        if (body[pos] == '[') ++depth;
        else if (body[pos] == ']') { if (--depth == 0) { ae = pos; break; } }
        ++pos;
    }
    if (ae == std::string::npos) throw std::runtime_error("unterminated embedding");

    std::vector<float> emb;
    std::string tok;
    for (size_t i = a + 1; i < ae; ++i) {
        char ch = body[i];
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
            if (!tok.empty()) { emb.push_back(parse_float_token(tok)); tok.clear(); }
        } else tok.push_back(ch);
    }
    if (!tok.empty()) emb.push_back(parse_float_token(tok));
    if (emb.empty()) throw std::runtime_error("empty embedding vector");
    return emb;
}

int parse_qdrant_count(const std::string& body) {
    auto cp = body.find("\"count\"");
    if (cp == std::string::npos) throw std::runtime_error("missing count");
    auto colon = body.find(':', cp);
    size_t s = colon + 1;
    while (s < body.size() && std::isspace(static_cast<unsigned char>(body[s]))) ++s;
    size_t e = s;
    while (e < body.size() && std::isdigit(static_cast<unsigned char>(body[e]))) ++e;
    return std::stoi(body.substr(s, e - s));
}

// ---------------------------------------------------------------------------
// Shell + Docker helpers
// ---------------------------------------------------------------------------

std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) { if (c == '\'') q += "'\\''"; else q.push_back(c); }
    q += "'";
    return q;
}

struct ShellResult { int code = 0; std::string output; };

ShellResult run_capture(const std::string& cmd) {
    auto* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) throw std::runtime_error("popen failed: " + cmd);
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    int raw = ::pclose(pipe);
    int code = (raw >= 0 && WIFEXITED(raw)) ? WEXITSTATUS(raw) : raw;
    return {code, out};
}

int run_shell(const std::string& cmd) {
    return std::system(cmd.c_str());
}

void compose_cmd(const Config& cfg, const std::string& args) {
    std::string cmd = "cd " + shell_quote(cfg.project_root) + " && docker compose " + args;
    if (run_shell(cmd) != 0)
        throw std::runtime_error("docker compose failed: " + args);
}

ShellResult compose_capture(const Config& cfg, const std::string& args) {
    return run_capture("cd " + shell_quote(cfg.project_root) + " && docker compose " + args);
}

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------

std::string format_float(float v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << v;
    return o.str();
}

std::string format_elapsed(int seconds) {
    int m = seconds / 60, s = seconds % 60;
    std::ostringstream o;
    o << std::setw(2) << std::setfill('0') << m << ":"
      << std::setw(2) << std::setfill('0') << s;
    return o.str();
}

std::atomic<SteadyTime> g_demo_start{SteadyClock::now()};

std::string timestamp_prefix() {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        SteadyClock::now() - g_demo_start.load()).count();
    return "[T+" + format_elapsed(static_cast<int>(elapsed)) + "]";
}

void demo_log(const char* color, const std::string& tag, const std::string& msg) {
    log_line(std::string(ansi::kDim) + timestamp_prefix() + ansi::kReset + " " +
             color + "[" + tag + "]" + ansi::kReset + " " + msg);
}

void info(const std::string& m)     { demo_log(ansi::kBlue, "INFO", m); }
void ok(const std::string& m)       { demo_log(ansi::kGreen, "OK", m); }
void warn(const std::string& m)     { demo_log(ansi::kYellow, "WARN", m); }
void fail(const std::string& m)     { demo_log(ansi::kRed, "FAIL", m); }
void phase_log(const std::string& m){ demo_log(ansi::kMagenta, "PHASE", m); }
void chaos(const std::string& m)    { demo_log(ansi::kRed, "CHAOS", m); }
void recover(const std::string& m)  { demo_log(ansi::kGreen, "RECOVER", m); }
void lock_log(const std::string& m) { demo_log(ansi::kYellow, "LOCK", m); }
void raft_log(const std::string& m) { demo_log(ansi::kCyan, "RAFT", m); }

void banner(const std::string& title) {
    log_line("");
    std::string bar(72, '=');
    log_line(std::string(ansi::kBold) + ansi::kMagenta + bar + ansi::kReset);
    log_line(std::string(ansi::kBold) + ansi::kMagenta + "  " + title + ansi::kReset);
    log_line(std::string(ansi::kBold) + ansi::kMagenta + bar + ansi::kReset);
    log_line("");
}

// ---------------------------------------------------------------------------
// Agent template + embedding
// ---------------------------------------------------------------------------

struct AgentTemplate {
    char        label = '?';
    std::string source_file;
    std::string text;
    std::vector<float> embedding;
    bool is_write = true;
};

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0f;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += double(a[i]) * double(b[i]);
        na  += double(a[i]) * double(a[i]);
        nb  += double(b[i]) * double(b[i]);
    }
    if (na <= 0 || nb <= 0) return 0.0f;
    return float(dot / (std::sqrt(na) * std::sqrt(nb)));
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream s;
    s << f.rdbuf();
    std::string t = s.str();
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' '))
        t.pop_back();
    return t;
}

AgentTemplate load_template(const Config& cfg, char label) {
    auto path = fs::path(cfg.input_dir) / (std::string(1, label) + ".json");
    if (!fs::exists(path)) throw std::runtime_error("missing " + path.string());
    std::string json = read_file(path);

    AgentTemplate t;
    t.label = label;
    t.source_file = path.filename().string();

    if (auto entry = parse_first_schedule_entry(json)) {
        t.text = parse_json_string(*entry, "payload");
        auto op = parse_json_string_opt(*entry, "operation", "write");
        t.is_write = (op != "read");
    } else {
        t.text = parse_json_string(json, "payload");
        auto op = parse_json_string_opt(json, "operation", "write");
        t.is_write = (op != "read");
    }
    return t;
}

std::vector<float> request_embedding(const Config& cfg, const std::string& text) {
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(cfg.model_id)
         << "\",\"input\":\"" << escape_json(text) << "\"}";
    auto resp = send_http_request(cfg.embedding_host, cfg.embedding_port,
                                  "POST", "/v1/embeddings", body.str());
    if (resp.status_code != 200)
        throw std::runtime_error("embedding failed: status " + std::to_string(resp.status_code));
    return parse_embedding_array(resp.body);
}

// ---------------------------------------------------------------------------
// Leader discovery
// ---------------------------------------------------------------------------

struct LeaderInfo {
    std::string address;
    std::string node_id;
    std::string service_name;
    int64_t     term = 0;
};

std::string service_for_node_id(const std::string& nid) {
    if (nid.rfind("node-", 0) != 0) return "";
    return "dscc-" + nid;
}

std::optional<LeaderInfo> discover_leader(const Config& cfg) {
    for (size_t i = 0; i < cfg.node_targets.size(); ++i) {
        auto chan = grpc::CreateChannel(cfg.node_targets[i], grpc::InsecureChannelCredentials());
        auto stub = dscc_raft::RaftService::NewStub(chan);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        dscc_raft::LeaderQuery req;
        dscc_raft::LeaderInfo resp;
        auto st = stub->GetLeader(&ctx, req, &resp);
        if (!st.ok()) continue;
        if (resp.is_leader() || !resp.leader_address().empty()) {
            LeaderInfo li;
            li.address = resp.leader_address();
            li.node_id = resp.leader_id();
            li.term    = resp.current_term();
            li.service_name = service_for_node_id(resp.leader_id());
            return li;
        }
    }
    return std::nullopt;
}

LeaderInfo wait_for_leader(const Config& cfg, int timeout_sec,
                           const std::string& different_from = {}) {
    auto deadline = SteadyClock::now() + std::chrono::seconds(timeout_sec);
    while (SteadyClock::now() < deadline) {
        auto obs = discover_leader(cfg);
        if (obs && !obs->address.empty() &&
            (different_from.empty() || obs->address != different_from))
            return *obs;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    throw std::runtime_error("timed out waiting for leader");
}

void wait_for_node_ready(const std::string& target, int timeout_sec) {
    auto deadline = SteadyClock::now() + std::chrono::seconds(timeout_sec);
    while (SteadyClock::now() < deadline) {
        auto chan = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
        auto stub = dscc_raft::RaftService::NewStub(chan);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        dscc_raft::LeaderQuery req;
        dscc_raft::LeaderInfo resp;
        if (stub->GetLeader(&ctx, req, &resp).ok()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    throw std::runtime_error("node " + target + " not ready");
}

// ---------------------------------------------------------------------------
// Cluster health snapshot
// ---------------------------------------------------------------------------

struct NodeHealth {
    std::string service_name;
    std::string target;
    bool        reachable = false;
    bool        is_leader = false;
    int64_t     term = 0;
};

std::vector<NodeHealth> poll_cluster(const Config& cfg) {
    std::vector<NodeHealth> nodes(cfg.node_targets.size());
    for (size_t i = 0; i < cfg.node_targets.size(); ++i) {
        nodes[i].service_name = cfg.node_service_names[i];
        nodes[i].target = cfg.node_targets[i];
        auto chan = grpc::CreateChannel(cfg.node_targets[i], grpc::InsecureChannelCredentials());
        auto stub = dscc_raft::RaftService::NewStub(chan);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
        dscc_raft::LeaderQuery req;
        dscc_raft::LeaderInfo resp;
        auto st = stub->GetLeader(&ctx, req, &resp);
        if (st.ok()) {
            nodes[i].reachable = true;
            nodes[i].is_leader = resp.is_leader();
            nodes[i].term = resp.current_term();
        }
    }
    return nodes;
}

void print_cluster_status(const Config& cfg) {
    auto nodes = poll_cluster(cfg);
    std::ostringstream line;
    line << "Cluster: ";
    for (auto& n : nodes) {
        if (n.is_leader)
            line << ansi::kBgGreen << ansi::kBold << " " << n.service_name << " [LEADER t" << n.term << "] " << ansi::kReset << " ";
        else if (n.reachable)
            line << ansi::kGreen << n.service_name << " [ok t" << n.term << "]" << ansi::kReset << " ";
        else
            line << ansi::kRed << n.service_name << " [DOWN]" << ansi::kReset << " ";
    }
    log_line(std::string(ansi::kDim) + timestamp_prefix() + ansi::kReset + " " +
             ansi::kCyan + "[CLUSTER]" + ansi::kReset + " " + line.str());
}

// ---------------------------------------------------------------------------
// Qdrant helpers
// ---------------------------------------------------------------------------

int qdrant_point_count(const Config& cfg) {
    auto resp = send_http_request(cfg.qdrant_host, cfg.qdrant_port, "POST",
        "/collections/" + cfg.collection + "/points/count", "{\"exact\":true}");
    if (resp.status_code != 200) return -1;
    return parse_qdrant_count(resp.body);
}

void reset_qdrant_collection(const Config& cfg, size_t dim) {
    send_http_request(cfg.qdrant_host, cfg.qdrant_port, "DELETE",
                      "/collections/" + cfg.collection);
    std::ostringstream body;
    body << "{\"vectors\":{\"size\":" << dim << ",\"distance\":\"Cosine\"}}";
    auto resp = send_http_request(cfg.qdrant_host, cfg.qdrant_port, "PUT",
                                  "/collections/" + cfg.collection, body.str());
    if (resp.status_code != 200 && resp.status_code != 201 && resp.status_code != 409)
        throw std::runtime_error("failed to create collection: " + std::to_string(resp.status_code));
}

// ---------------------------------------------------------------------------
// Workload stats (thread-safe accumulator)
// ---------------------------------------------------------------------------

struct WorkloadStats {
    std::mutex mu;
    int total_ops         = 0;
    int successful_ops    = 0;
    int failed_ops        = 0;
    int blocked_ops       = 0;
    int64_t total_wait_ms = 0;
    int64_t max_wait_ms   = 0;
    int64_t total_rpc_ms  = 0;

    void record(bool success, int64_t wait_ms, int64_t rpc_ms, bool was_blocked) {
        std::lock_guard<std::mutex> lk(mu);
        ++total_ops;
        if (success) ++successful_ops; else ++failed_ops;
        if (was_blocked) ++blocked_ops;
        total_wait_ms += wait_ms;
        max_wait_ms = std::max(max_wait_ms, wait_ms);
        total_rpc_ms += rpc_ms;
    }

    void print_summary(const std::string& label) {
        std::lock_guard<std::mutex> lk(mu);
        log_line("");
        log_line(std::string(ansi::kCyan) + "  --- " + label + " Stats ---" + ansi::kReset);
        log_line("  Total ops:      " + std::to_string(total_ops));
        log_line("  Successful:     " + std::to_string(successful_ops));
        log_line("  Failed:         " + std::to_string(failed_ops));
        log_line("  Blocked (wait): " + std::to_string(blocked_ops));
        if (total_ops > 0) {
            log_line("  Avg wait:       " + std::to_string(total_wait_ms / total_ops) + "ms");
            log_line("  Max wait:       " + std::to_string(max_wait_ms) + "ms");
            log_line("  Avg RPC:        " + std::to_string(total_rpc_ms / total_ops) + "ms");
        }
        log_line("");
    }
};

// ---------------------------------------------------------------------------
// Continuous workload driver
// ---------------------------------------------------------------------------

struct WorkloadConfig {
    const Config*                       cfg;
    const std::vector<AgentTemplate>*   templates;
    std::atomic<bool>*                  stop_flag;
    WorkloadStats*                      stats;
    int                                 interval_ms;
};

void workload_thread(WorkloadConfig wc, size_t template_idx) {
    const auto& tmpl = (*wc.templates)[template_idx];
    auto channel = grpc::CreateChannel(wc.cfg->dscc_target, grpc::InsecureChannelCredentials());
    int seq = 0;

    while (!wc.stop_flag->load(std::memory_order_relaxed)) {
        std::string agent_id = "demo-agent-" + std::string(1, tmpl.label) + "-" + std::to_string(seq++);
        std::string op_name = tmpl.is_write ? "write" : "read";

        dscc::AcquireRequest req;
        req.set_agent_id(agent_id);
        req.set_payload_text(tmpl.text);
        req.set_source_file(tmpl.source_file);
        req.set_timestamp_unix_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        req.set_operation_type(
            tmpl.is_write ? dscc::AcquireRequest::OPERATION_TYPE_WRITE
                          : dscc::AcquireRequest::OPERATION_TYPE_READ);
        for (float v : tmpl.embedding) req.add_embedding(v);

        auto stub = dscc::LockService::NewStub(channel);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
        dscc::AcquireResponse resp;

        auto t0 = SteadyClock::now();
        auto status = stub->AcquireGuard(&ctx, req, &resp);
        auto rpc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            SteadyClock::now() - t0).count();

        bool blocked = resp.lock_wait_ms() > 0;
        wc.stats->record(status.ok() && resp.granted(),
                         resp.lock_wait_ms(), rpc_ms, blocked);

        if (status.ok() && resp.granted()) {
            std::ostringstream msg;
            msg << "Agent " << tmpl.label << " " << op_name << " #" << seq
                << " granted on " << resp.serving_node_id()
                << " | wait=" << resp.lock_wait_ms() << "ms"
                << " rpc=" << rpc_ms << "ms";
            if (blocked) {
                msg << " blocked_by=" << resp.blocking_agent_id()
                    << " sim=" << format_float(resp.blocking_similarity_score());
                lock_log(msg.str());
            } else {
                ok(msg.str());
            }
        } else {
            std::ostringstream msg;
            msg << "Agent " << tmpl.label << " " << op_name << " #" << seq << " FAILED";
            if (!status.ok()) msg << " gRPC=" << status.error_message();
            else msg << " denied: " << resp.message();
            fail(msg.str());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(wc.interval_ms));
    }
}

// ---------------------------------------------------------------------------
// Stack lifecycle
// ---------------------------------------------------------------------------

void set_compose_env(const Config& cfg) {
    ::setenv("EMBEDDING_IMAGE", cfg.embedding_image.c_str(), 1);
    ::setenv("EMBEDDING_MODEL_ID", cfg.model_id.c_str(), 1);
    ::setenv("INFINITY_IMAGE", cfg.embedding_image.c_str(), 1);
    ::setenv("INFINITY_MODEL_ID", cfg.model_id.c_str(), 1);
    ::setenv("QDRANT_COLLECTION", cfg.collection.c_str(), 1);

    std::ostringstream theta;
    theta << std::fixed << std::setprecision(2) << cfg.theta;
    ::setenv("DSCC_THETA", theta.str().c_str(), 1);
    ::setenv("DSCC_LOCK_HOLD_MS", std::to_string(cfg.lock_hold_ms).c_str(), 1);
}

void start_stack(const Config& cfg) {
    set_compose_env(cfg);
    compose_cmd(cfg, "up -d --build qdrant embedding-service "
                     "dscc-node-1 dscc-node-2 dscc-node-3 dscc-node-4 dscc-node-5 dscc-proxy");
}

void stop_stack(const Config& cfg) {
    auto cmd = "cd " + shell_quote(cfg.project_root) + " && docker compose down";
    run_shell(cmd);
}

void wait_for_qdrant(const Config& cfg) {
    for (int i = 0; i < 120; ++i) {
        try {
            auto r = send_http_request(cfg.qdrant_host, cfg.qdrant_port, "GET", "/collections");
            if (r.status_code >= 200 && r.status_code < 300) { info("Qdrant ready"); return; }
        } catch (...) {}
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    throw std::runtime_error("Qdrant not ready");
}

void wait_for_embedding(const Config& cfg) {
    std::vector<std::string> endpoints = {"/api/tags", "/v1/models", "/"};
    for (int i = 0; i < 180; ++i) {
        for (auto& ep : endpoints) {
            try {
                auto r = send_http_request(cfg.embedding_host, cfg.embedding_port, "GET", ep);
                if (r.status_code >= 200 && r.status_code < 300) {
                    info("Embedding service ready");
                    return;
                }
            } catch (...) {}
        }
        if (i % 10 == 0) info("Waiting for embedding service...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    throw std::runtime_error("embedding service not ready");
}

void ensure_model(const Config& cfg) {
    info("Pulling model " + cfg.model_id + "...");
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(cfg.model_id) << "\",\"stream\":false}";
    auto r = send_http_request(cfg.embedding_host, cfg.embedding_port,
                               "POST", "/api/pull", body.str());
    if (r.status_code != 200)
        throw std::runtime_error("model pull failed: " + std::to_string(r.status_code));
    ok("Model ready");
}

void wait_for_proxy(const Config& cfg) {
    auto chan = grpc::CreateChannel(cfg.dscc_target, grpc::InsecureChannelCredentials());
    auto stub = dscc::LockService::NewStub(chan);
    for (int i = 0; i < 60; ++i) {
        grpc::ClientContext ctx;
        dscc::PingRequest req;
        dscc::PingResponse resp;
        req.set_from_node("e2e-demo");
        if (stub->Ping(&ctx, req, &resp).ok()) { info("Proxy reachable"); return; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    throw std::runtime_error("proxy not ready");
}

// ---------------------------------------------------------------------------
// Config loader
// ---------------------------------------------------------------------------

Config load_config() {
    Config c;
    if (auto* v = std::getenv("EMBEDDING_IMAGE"))    c.embedding_image = v;
    else if (auto* v2 = std::getenv("INFINITY_IMAGE")) c.embedding_image = v2;
    if (auto* v = std::getenv("EMBEDDING_MODEL_ID")) c.model_id = v;
    else if (auto* v2 = std::getenv("INFINITY_MODEL_ID")) c.model_id = v2;
    if (auto* v = std::getenv("QDRANT_COLLECTION"))  c.collection = v;
    if (auto* v = std::getenv("DSCC_THETA"))         c.theta = std::strtof(v, nullptr);
    if (auto* v = std::getenv("DSCC_LOCK_HOLD_MS"))  c.lock_hold_ms = std::atoi(v);
    if (auto* v = std::getenv("E2E_TEARDOWN"))       c.teardown_on_exit = std::string(v) == "1";
    if (auto* v = std::getenv("DEMO_OP_INTERVAL_MS"))c.op_interval_ms = std::atoi(v);
    if (auto* v = std::getenv("DEMO_DURATION_SEC"))  c.duration_sec = std::atoi(v);
    return c;
}

// ---------------------------------------------------------------------------
// Phase definitions
// ---------------------------------------------------------------------------

struct Phase {
    int         start_sec;
    std::string name;
    std::string description;
    std::function<void(const Config&, WorkloadStats&)> action;
};

void phase_steady_state(const Config& cfg, WorkloadStats& stats) {
    banner("PHASE 1: STEADY-STATE OPERATION");
    phase_log("All 5 nodes healthy. Continuous workload running.");
    phase_log("Observe: semantic conflicts cause serialization, distinct requests run in parallel.");
    print_cluster_status(cfg);
}

void phase_leader_kill(const Config& cfg, WorkloadStats& stats) {
    banner("PHASE 2: LEADER FAILOVER");
    auto leader = wait_for_leader(cfg, 8);
    chaos("Killing current leader: " + leader.service_name +
          " (term " + std::to_string(leader.term) + ")");
    print_cluster_status(cfg);

    compose_cmd(cfg, "stop " + leader.service_name);
    chaos(leader.service_name + " stopped. Workload continues against proxy...");
    print_cluster_status(cfg);

    phase_log("Waiting for new leader election...");
    auto new_leader = wait_for_leader(cfg, 15, leader.address);
    recover("New leader elected: " + new_leader.service_name +
            " (term " + std::to_string(new_leader.term) + ")");
    print_cluster_status(cfg);
    stats.print_summary("Post-Failover");
}

void phase_old_leader_rejoin(const Config& cfg, WorkloadStats& stats) {
    banner("PHASE 3: NODE RECOVERY & LOG CATCH-UP");

    auto nodes = poll_cluster(cfg);
    std::string down_service;
    std::string down_target;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].reachable) {
            down_service = nodes[i].service_name;
            down_target  = nodes[i].target;
            break;
        }
    }

    if (down_service.empty()) {
        phase_log("All nodes already up; restarting node-5 for demonstration.");
        down_service = "dscc-node-5";
        down_target  = "127.0.0.1:50055";
        compose_cmd(cfg, "stop " + down_service);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    recover("Restarting " + down_service + "...");
    compose_cmd(cfg, "start " + down_service);
    wait_for_node_ready(down_target, 15);
    recover(down_service + " rejoined the cluster and is catching up on Raft log.");
    print_cluster_status(cfg);

    std::this_thread::sleep_for(std::chrono::seconds(3));
    print_cluster_status(cfg);
    stats.print_summary("Post-Recovery");
}

void phase_quorum_degradation(const Config& cfg, WorkloadStats& stats) {
    banner("PHASE 4: QUORUM COLLAPSE — SYSTEM HALT");

    auto leader = wait_for_leader(cfg, 10);
    auto nodes = poll_cluster(cfg);

    std::vector<std::string> followers;
    for (auto& n : nodes) {
        if (n.reachable && !n.is_leader) followers.push_back(n.service_name);
    }

    if (followers.size() < 3) {
        fail("Need at least 3 reachable followers for this scenario; have " +
             std::to_string(followers.size()) + ". Aborting phase.");
        return;
    }

    phase_log("Leader: " + leader.service_name + " (term " + std::to_string(leader.term) + ")");
    phase_log("Raft quorum for a 5-node cluster = 3. Killing 3 followers will leave 2/5 nodes — below quorum.");
    print_cluster_status(cfg);

    chaos("Stopping follower #1: " + followers[0] + "  ->  4/5 nodes (still above quorum)");
    compose_cmd(cfg, "stop " + followers[0]);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    print_cluster_status(cfg);

    chaos("Stopping follower #2: " + followers[1] + "  ->  3/5 nodes (bare quorum)");
    compose_cmd(cfg, "stop " + followers[1]);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    print_cluster_status(cfg);

    chaos("Stopping follower #3: " + followers[2] + "  ->  2/5 nodes (QUORUM LOST)");
    compose_cmd(cfg, "stop " + followers[2]);
    print_cluster_status(cfg);

    phase_log("Cluster is BELOW QUORUM. Raft cannot replicate log entries; all writes will block and eventually fail.");
    phase_log("Observing halted state for 30 seconds — expect timeouts and zero successful writes...");

    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        print_cluster_status(cfg);
    }

    stats.print_summary("Post-Quorum-Loss (system halted)");
    phase_log("Scenario complete — system remains halted as expected. Ending demo.");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

}  // namespace

int main() {
    Config cfg;
    bool stack_started = false;

    try {
        cfg = load_config();

        banner("DISTRIBUTED SEMANTIC LOCK MANAGER — LIVE DEMO");
        info("Project root:     " + cfg.project_root);
        info("Embedding model:  " + cfg.model_id);
        info("Theta:            " + format_float(cfg.theta));
        info("Lock hold:        " + std::to_string(cfg.lock_hold_ms) + "ms");
        info("Op interval:      " + std::to_string(cfg.op_interval_ms) + "ms");
        info("Demo duration:    " + std::to_string(cfg.duration_sec) + "s (" +
             format_elapsed(cfg.duration_sec) + ")");
        info("Collection:       " + cfg.collection);
        log_line("");

        // -- Bring up Docker stack --
        banner("STACK STARTUP");
        info("Starting Docker Compose stack (5 nodes, proxy, Qdrant, embedding)...");
        start_stack(cfg);
        stack_started = true;

        wait_for_qdrant(cfg);
        wait_for_embedding(cfg);
        ensure_model(cfg);
        wait_for_proxy(cfg);

        auto leader = wait_for_leader(cfg, 15);
        raft_log("Initial leader: " + leader.service_name +
                 " at " + leader.address + " (term " + std::to_string(leader.term) + ")");
        print_cluster_status(cfg);

        // -- Load agent templates and compute embeddings --
        banner("AGENT PREPARATION");
        std::vector<char> labels = {'A', 'B', 'C', 'D', 'E'};
        std::vector<AgentTemplate> templates;
        for (char l : labels) {
            auto t = load_template(cfg, l);
            info("Embedding " + t.source_file + "...");
            t.embedding = request_embedding(cfg, t.text);
            info("  " + t.source_file + ": dim=" + std::to_string(t.embedding.size()) +
                 " op=" + std::string(t.is_write ? "write" : "read"));
            templates.push_back(std::move(t));
        }

        // Print similarity matrix
        log_line("");
        log_line(std::string(ansi::kCyan) + "  Pairwise Cosine Similarity (theta=" +
                 format_float(cfg.theta) + "):" + ansi::kReset);
        std::ostringstream hdr;
        hdr << "         ";
        for (auto& t : templates) hdr << std::setw(8) << t.label;
        log_line(hdr.str());
        for (size_t i = 0; i < templates.size(); ++i) {
            std::ostringstream row;
            row << "    " << templates[i].label << "    ";
            for (size_t j = 0; j < templates.size(); ++j) {
                float sim = cosine_similarity(templates[i].embedding, templates[j].embedding);
                if (i != j && sim >= cfg.theta)
                    row << ansi::kRed << std::setw(8) << format_float(sim) << ansi::kReset;
                else
                    row << std::setw(8) << format_float(sim);
            }
            log_line(row.str());
        }
        log_line(std::string(ansi::kDim) + "  (red = above theta, will conflict)" + ansi::kReset);
        log_line("");

        // -- Reset Qdrant --
        reset_qdrant_collection(cfg, templates.front().embedding.size());
        ok("Qdrant collection reset.");

        // -- Define phases --
        // Times are relative to demo start (when workload begins).
        // Total runtime ~5 minutes. The demo exits as soon as the final phase completes.
        std::vector<Phase> phases = {
            {0,   "Steady State",
             "Brief baseline: all 5 nodes healthy. Observe semantic conflict detection.",
             phase_steady_state},
            {20,  "Leader Failover",
             "Kill the Raft leader mid-operation. Watch election and automatic recovery.",
             phase_leader_kill},
            {90,  "Node Recovery",
             "Restart the killed node. Watch it rejoin and catch up on Raft log entries.",
             phase_old_leader_rejoin},
            {180, "Quorum Collapse",
             "Kill 3 followers to breach quorum. System halts because Raft cannot commit.",
             phase_quorum_degradation},
        };

        // -- Print demo roadmap --
        banner("DEMO ROADMAP");
        for (auto& p : phases) {
            log_line(std::string(ansi::kBold) + "  " + format_elapsed(p.start_sec) +
                     "  " + p.name + ansi::kReset);
            log_line(std::string(ansi::kDim) + "         " + p.description + ansi::kReset);
        }
        log_line(std::string(ansi::kBold) +
                 "  Demo ends immediately after 'Quorum Collapse' completes (≈ " +
                 format_elapsed(cfg.duration_sec) + " total)" + ansi::kReset);
        log_line("");

        // -- Start continuous workload --
        banner("WORKLOAD START");
        info("Launching " + std::to_string(templates.size()) + " agent threads, interval=" +
             std::to_string(cfg.op_interval_ms) + "ms");

        std::atomic<bool> stop_flag{false};
        WorkloadStats stats;
        g_demo_start.store(SteadyClock::now());
        auto demo_start = SteadyClock::now();

        std::vector<std::thread> workers;
        for (size_t i = 0; i < templates.size(); ++i) {
            WorkloadConfig wc{&cfg, &templates, &stop_flag, &stats, cfg.op_interval_ms};
            workers.emplace_back(workload_thread, wc, i);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        // -- Phase execution loop --
        size_t next_phase = 0;
        int next_health_sec = 15;

        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                SteadyClock::now() - demo_start).count();

            if (elapsed >= cfg.duration_sec) break;

            if (next_phase < phases.size() &&
                elapsed >= phases[next_phase].start_sec) {
                auto& p = phases[next_phase];
                try {
                    p.action(cfg, stats);
                } catch (const std::exception& ex) {
                    fail(std::string("Phase '") + p.name + "' error: " + ex.what());
                }
                ++next_phase;
                // End the demo as soon as the final phase (quorum collapse) completes.
                if (next_phase >= phases.size()) break;
                next_health_sec = static_cast<int>(elapsed) + 15;
                continue;
            }

            if (static_cast<int>(elapsed) >= next_health_sec) {
                print_cluster_status(cfg);
                int count = qdrant_point_count(cfg);
                if (count >= 0) info("Qdrant points: " + std::to_string(count));
                next_health_sec += 30;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // -- Shutdown workload --
        banner("DEMO COMPLETE");
        info("Stopping workload threads...");
        stop_flag.store(true);
        for (auto& w : workers) w.join();

        stats.print_summary("Overall Demo");
        print_cluster_status(cfg);

        int final_count = qdrant_point_count(cfg);
        if (final_count >= 0)
            ok("Final Qdrant point count: " + std::to_string(final_count));

        if (cfg.teardown_on_exit) {
            info("Tearing down stack...");
            stop_stack(cfg);
        } else {
            info("Stack left running. Use 'docker compose down' to clean up.");
        }

        ok("Demo finished successfully.");
        return 0;

    } catch (const std::exception& ex) {
        fail(std::string("Fatal: ") + ex.what());
        if (stack_started && cfg.teardown_on_exit) stop_stack(cfg);
        return 1;
    }
}
