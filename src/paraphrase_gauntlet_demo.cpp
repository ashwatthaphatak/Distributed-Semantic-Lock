// Paraphrase Gauntlet × Model Matrix Demo
//
// Runs the Paraphrase Gauntlet scenario (case 11) and Cross-Domain Flood
// scenario (case 12) against the live containerized DSCC stack, sweeping all
// three embedding models at a fixed θ.  For each model, constructs a confusion
// matrix treating conflict detection as a binary classification problem and
// writes structured results to a single JSON file.
//
// The full Docker stack (proxy, 5 dscc-nodes, Qdrant, embedding-service) is
// started ONCE.  All three embedding models are served by a single Ollama
// container — the model is selected per-request via the API "model" field.
// No containers are restarted between model runs; Qdrant is flushed between
// scenarios so stale vectors from one model don't affect the next.
//
// Build target: dscc-paraphrase-gauntlet-demo
// Usage:        ./build/dscc-paraphrase-gauntlet-demo
//
// Environment overrides (all optional):
//   DSLM_GAUNTLET_THETA  — similarity threshold (default: 0.75)
//   DSLM_GAUNTLET_OUTPUT — output JSON path      (default: logs/paraphrase_gauntlet_results_<ts>.json)
//   E2E_TEARDOWN         — 1 to tear down         (default: 1)
//   EMBEDDING_HOST       — embedding host          (default: 127.0.0.1)
//   EMBEDDING_PORT       — embedding port          (default: 7997)

#include "dscc.grpc.pb.h"
#include "dscc_raft.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Embedding model profiles — same order as kProfiles in benchmark_runner.cpp
// ---------------------------------------------------------------------------

struct EmbeddingProfile {
    std::string name;
    std::string model_id;
    std::string image;
};

static const std::array<EmbeddingProfile, 3> kProfiles = {{
    {"ollama", "all-minilm:latest",       "ollama/ollama:latest"},
    {"bge",    "bge-m3:latest",           "ollama/ollama:latest"},
    {"qwen",   "qwen3-embedding:0.6b",    "ollama/ollama:latest"},
}};

static constexpr float kDefaultTheta = 0.75f;
static const std::string kGauntletCollection = "dscc_gauntlet_e2e";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    std::string project_root = DSLM_PROJECT_ROOT;
    std::string input_dir    = DSLM_PROJECT_ROOT "/demo_inputs";
    std::string embedding_image = "ollama/ollama:latest";
    std::string model_id        = "all-minilm:latest";
    std::string embedding_profile = "ollama";
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
    float theta = kDefaultTheta;
    int lock_hold_ms = 750;
    bool teardown_on_exit = true;
    std::string output_path;
};

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct HttpResponse {
    int status_code = 0;
    std::string body;
};

struct ShellCommandResult {
    int exit_code = 0;
    std::string output;
};

struct TemplateDocument {
    std::string template_id;
    std::string source_file;
    std::string text;
    std::vector<float> embedding;
};

enum class OperationType { kWrite, kRead };

struct BenchmarkOperation {
    std::string agent_id;
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
    int64_t submit_ms  = 0;
    int64_t finish_ms  = 0;
    int64_t elapsed_ms = 0;
};

// ---------------------------------------------------------------------------
// Confusion matrix and classification metrics
//
// Conflict detection is modelled as a binary classifier:
//
//   Ground truth: a pair of requests "should conflict" if their embedding
//   cosine similarity >= θ (paraphrase / same-concept pairs).  They "should
//   not conflict" if similarity < θ (cross-domain pairs).
//
//   Prediction: the system "predicted conflict" if it serialised the two
//   requests (their lock-hold intervals do NOT overlap on the server
//   timeline).  It "predicted no conflict" if both ran in parallel (their
//   lock-hold intervals DO overlap).
//
//               Predicted: Conflict     Predicted: No Conflict
//  Actual: Conflict       TP                    FN
//  Actual: No Conflict    FP                    TN
//
//   TP — true positive:  should conflict, system correctly serialised.
//   FN — false negative: should conflict, but system allowed parallel
//         execution (a serialisation violation).
//   FP — false positive: should NOT conflict, but system unnecessarily
//         blocked the second request (wasted concurrency).
//   TN — true negative:  should NOT conflict, system correctly allowed
//         concurrent execution.
// ---------------------------------------------------------------------------

struct ConfusionMatrix {
    int tp = 0;
    int fn = 0;
    int fp = 0;
    int tn = 0;
};

struct ClassificationMetrics {
    double accuracy    = 0.0;
    double precision   = 0.0;
    double recall      = 0.0;
    double f1          = 0.0;
    double serialization_score       = 0.0;
    double distinct_parallelism_rate = 0.0;
    double false_positive_rate       = 0.0;
};

struct PairOutcome {
    std::string scenario;
    std::string text_a;
    std::string text_b;
    std::string agent_a;
    std::string agent_b;
    float cosine_similarity = 0.0f;
    bool expected_conflict  = false;
    bool observed_serialized = false;
    bool pass = false;
};

struct ModelRunResult {
    std::string profile_name;
    std::string model_id;
    bool success = false;
    std::string error_message;
    ConfusionMatrix matrix;
    ClassificationMetrics metrics;
    std::vector<PairOutcome> pair_details;
    int paraphrase_ops = 0;
    int cross_domain_ops = 0;
    size_t embedding_dim = 0;
};

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

void info(const std::string& msg) { std::cout << "[INFO] " << msg << std::endl; }
void warn(const std::string& msg) { std::cout << "[WARN] " << msg << std::endl; }

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q.push_back(c);
    }
    q += "'";
    return q;
}

std::string trim_copy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lowercase_copy(std::string t) {
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return t;
}

std::string getenv_or(const char* key, const std::string& fb) {
    const char* v = std::getenv(key);
    return v ? v : fb;
}

bool getenv_flag(const char* key, bool fb) {
    const char* v = std::getenv(key);
    if (!v) return fb;
    std::string s = lowercase_copy(v);
    return s == "1" || s == "true" || s == "yes";
}

std::string format_float(double v, int p) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(p) << v;
    return o.str();
}

Config load_config() {
    Config c;
    c.embedding_host = getenv_or("EMBEDDING_HOST", c.embedding_host);
    c.embedding_port = getenv_or("EMBEDDING_PORT", c.embedding_port);
    c.teardown_on_exit = getenv_flag("E2E_TEARDOWN", true);
    c.output_path = getenv_or("DSLM_GAUNTLET_OUTPUT", "");
    {
        std::string v = getenv_or("DSLM_GAUNTLET_THETA", "");
        if (!v.empty()) c.theta = std::stof(v);
    }
    return c;
}

// ---------------------------------------------------------------------------
// HTTP client (raw sockets, identical pattern to benchmark_runner.cpp)
// ---------------------------------------------------------------------------

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

HttpResponse send_http_request(const std::string& host, const std::string& port,
                               const std::string& method, const std::string& target,
                               const std::string& body = {},
                               const std::string& ct = "application/json") {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* addrs = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &addrs) != 0)
        fail("DNS failed for " + host + ":" + port);

    int fd = -1;
    for (auto* a = addrs; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd); fd = -1;
    }
    ::freeaddrinfo(addrs);
    if (fd < 0) fail("connect failed for " + host + ":" + port);

    std::ostringstream req;
    req << method << " " << target << " HTTP/1.1\r\nHost: " << host << ":" << port
        << "\r\nConnection: close\r\n";
    if (!body.empty())
        req << "Content-Type: " << ct << "\r\nContent-Length: " << body.size() << "\r\n";
    req << "\r\n" << body;
    if (!send_all(fd, req.str())) { ::close(fd); fail("send failed"); }

    std::string raw;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) { ::close(fd); fail("recv failed"); }
        if (n == 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) fail("malformed HTTP response");
    std::istringstream hdr(raw.substr(0, hdr_end));
    std::string ver; int code = 0;
    hdr >> ver >> code;
    return {code, raw.substr(hdr_end + 4)};
}

// ---------------------------------------------------------------------------
// JSON / embedding parsing
// ---------------------------------------------------------------------------

float parse_float_token(const std::string& tok) {
    char* end = nullptr;
    float v = std::strtof(tok.c_str(), &end);
    if (end == tok.c_str()) fail("bad float: " + tok);
    return v;
}

std::vector<float> parse_embedding_array(const std::string& body) {
    auto kp = body.find("\"embedding\"");
    if (kp == std::string::npos) fail("missing embedding field");
    auto as = body.find('[', kp);
    if (as == std::string::npos) fail("malformed embedding");
    size_t pos = as + 1; int depth = 1; size_t ae = std::string::npos;
    while (pos < body.size()) {
        if (body[pos] == '[') ++depth;
        else if (body[pos] == ']') { if (--depth == 0) { ae = pos; break; } }
        ++pos;
    }
    if (ae == std::string::npos) fail("unterminated embedding array");
    std::vector<float> vals;
    std::string tok;
    for (size_t i = as + 1; i < ae; ++i) {
        char ch = body[i];
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
            if (!tok.empty()) { vals.push_back(parse_float_token(tok)); tok.clear(); }
        } else tok.push_back(ch);
    }
    if (!tok.empty()) vals.push_back(parse_float_token(tok));
    if (vals.empty()) fail("empty embedding vector");
    return vals;
}

std::string read_text_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) fail("could not open " + p.string());
    std::ostringstream buf; buf << f.rdbuf();
    std::string t = buf.str();
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' '))
        t.pop_back();
    if (t.empty()) fail("file empty: " + p.string());
    return t;
}

std::string parse_json_string_field(const std::string& body, const std::string& key) {
    std::string qk = "\"" + key + "\"";
    auto kp = body.find(qk);
    if (kp == std::string::npos) fail("missing field: " + key);
    auto col = body.find(':', kp + qk.size());
    auto qt  = body.find('"', col + 1);
    if (col == std::string::npos || qt == std::string::npos) fail("bad field: " + key);
    std::string val;
    bool esc = false;
    for (size_t i = qt + 1; i < body.size(); ++i) {
        char ch = body[i];
        if (esc) { val.push_back(ch == 'n' ? '\n' : ch); esc = false; continue; }
        if (ch == '\\') { esc = true; continue; }
        if (ch == '"') return val;
        val.push_back(ch);
    }
    fail("unterminated string field: " + key);
}

std::vector<std::string> parse_payload_schedule_entries(const std::string& body) {
    auto kp = body.find("\"payload_schedule\"");
    if (kp == std::string::npos) return {};
    auto as = body.find('[', kp);
    if (as == std::string::npos) fail("bad payload_schedule");
    std::vector<std::string> entries;
    bool in_str = false, esc = false;
    int od = 0; size_t os = std::string::npos;
    for (size_t i = as + 1; i < body.size(); ++i) {
        char ch = body[i];
        if (esc) { esc = false; continue; }
        if (ch == '\\') { esc = true; continue; }
        if (ch == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (ch == '{') { if (od == 0) os = i; ++od; }
        else if (ch == '}') { if (od > 0 && --od == 0 && os != std::string::npos) entries.push_back(body.substr(os, i - os + 1)); }
        else if (ch == ']' && od == 0) break;
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Cosine similarity
// ---------------------------------------------------------------------------

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0f;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += double(a[i]) * double(b[i]);
        na  += double(a[i]) * double(a[i]);
        nb  += double(b[i]) * double(b[i]);
    }
    if (na <= 0.0 || nb <= 0.0) return 0.0f;
    return static_cast<float>(std::max(-1.0, std::min(1.0, dot / (std::sqrt(na) * std::sqrt(nb)))));
}

bool intervals_overlap(int64_t sa, int64_t ea, int64_t sb, int64_t eb) {
    if (sa <= 0 || ea <= 0 || sb <= 0 || eb <= 0) return false;
    return std::max(sa, sb) < std::min(ea, eb);
}

// ---------------------------------------------------------------------------
// Shell / Docker helpers
// ---------------------------------------------------------------------------

ShellCommandResult run_shell_capture(const std::string& cmd) {
    std::string wrapped = cmd + " 2>&1";
    FILE* p = ::popen(wrapped.c_str(), "r");
    if (!p) fail("popen failed: " + cmd);
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int raw = ::pclose(p);
    int ec = raw;
    if (raw >= 0 && WIFEXITED(raw)) ec = WEXITSTATUS(raw);
    return {ec, out};
}

void run_shell_or_throw(const std::string& cmd, const std::string& err) {
    info("Executing: " + cmd);
    auto r = run_shell_capture(cmd);
    if (r.exit_code != 0) fail(err + "\n" + r.output);
}

std::string compose_command(const Config& c, const std::string& args) {
    return "cd " + shell_quote(c.project_root) + " && docker compose " + args;
}

// ---------------------------------------------------------------------------
// Service health-check / wait functions
// ---------------------------------------------------------------------------

void wait_for_http_service(const std::string& name, const std::string& host,
                           const std::string& port,
                           const std::vector<std::string>& targets,
                           int max_attempts = 120, int sleep_ms = 1000) {
    for (int a = 1; a <= max_attempts; ++a) {
        for (auto& t : targets) {
            try {
                auto r = send_http_request(host, port, "GET", t);
                if (r.status_code >= 200 && r.status_code < 300) return;
            } catch (...) {}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    fail("timed out waiting for " + name);
}

void wait_for_embedding_service(const Config& c) {
    wait_for_http_service("embedding service", c.embedding_host, c.embedding_port,
                          {"/api/tags", "/v1/models", "/"});
}

void wait_for_dscc_service(const Config& c, int max = 90, int ms = 1000) {
    auto ch = grpc::CreateChannel(c.dscc_target, grpc::InsecureChannelCredentials());
    auto stub = dscc::LockService::NewStub(ch);
    for (int a = 1; a <= max; ++a) {
        grpc::ClientContext ctx;
        dscc::PingRequest req; dscc::PingResponse resp;
        req.set_from_node("gauntlet-demo");
        if (stub->Ping(&ctx, req, &resp).ok()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    fail("timed out waiting for DSCC proxy");
}

void wait_for_leader(const Config& c, std::chrono::milliseconds timeout) {
    auto deadline = SteadyClock::now() + timeout;
    while (SteadyClock::now() < deadline) {
        for (auto& t : c.node_targets) {
            auto ch = grpc::CreateChannel(t, grpc::InsecureChannelCredentials());
            auto stub = dscc_raft::RaftService::NewStub(ch);
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
            dscc_raft::LeaderQuery req; dscc_raft::LeaderInfo resp;
            if (stub->GetLeader(&ctx, req, &resp).ok() && !resp.leader_address().empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fail("timed out waiting for leader");
}

// ---------------------------------------------------------------------------
// Qdrant collection management
// ---------------------------------------------------------------------------

void reset_qdrant_collection(const Config& c, const std::string& col) {
    auto r = send_http_request(c.qdrant_host, c.qdrant_port, "DELETE", "/collections/" + col);
    if (r.status_code != 200 && r.status_code != 202 && r.status_code != 404)
        fail("failed to reset Qdrant collection");
}

void create_qdrant_collection(const Config& c, const std::string& col, size_t dim) {
    std::ostringstream body;
    body << "{\"vectors\":{\"size\":" << dim << ",\"distance\":\"Cosine\"}}";
    auto r = send_http_request(c.qdrant_host, c.qdrant_port, "PUT", "/collections/" + col, body.str());
    if (r.status_code != 200 && r.status_code != 201 && r.status_code != 409)
        fail("failed to create Qdrant collection: " + r.body);
}

int qdrant_point_count(const Config& c, const std::string& col) {
    auto r = send_http_request(c.qdrant_host, c.qdrant_port, "POST",
                               "/collections/" + col + "/points/count", "{\"exact\":true}");
    if (r.status_code != 200) fail("Qdrant count failed");
    auto cp = r.body.find("\"count\"");
    auto colon = r.body.find(':', cp);
    size_t vs = colon + 1;
    while (vs < r.body.size() && std::isspace(static_cast<unsigned char>(r.body[vs]))) ++vs;
    size_t ve = vs;
    while (ve < r.body.size() && std::isdigit(static_cast<unsigned char>(r.body[ve]))) ++ve;
    return std::stoi(r.body.substr(vs, ve - vs));
}

// ---------------------------------------------------------------------------
// Embedding request
// ---------------------------------------------------------------------------

std::vector<float> request_embedding(const Config& c, const std::string& text) {
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(c.model_id)
         << "\",\"input\":\"" << escape_json(text) << "\"}";
    auto r = send_http_request(c.embedding_host, c.embedding_port, "POST", "/v1/embeddings", body.str());
    if (r.status_code != 200)
        fail("embedding request failed (status " + std::to_string(r.status_code) + ")");
    return parse_embedding_array(r.body);
}

// ---------------------------------------------------------------------------
// Docker stack lifecycle
//
// All three embedding models (all-minilm, bge-m3, qwen3-embedding) are served
// by a SINGLE Ollama container.  Ollama is a multi-model server: the model is
// selected per-request via the "model" field in the /v1/embeddings POST body.
// No container restart is needed between models — we just switch config.model_id
// and the next embedding request automatically uses the new model.
//
// The full cluster (proxy, 5 dscc-nodes, Qdrant, embedding-service) is started
// once.  Between model runs nothing is restarted; only the Qdrant collection
// is flushed so stale vectors from a previous model don't pollute the next run.
// ---------------------------------------------------------------------------

void set_stack_environment(const Config& c) {
    ::setenv("EMBEDDING_IMAGE",    c.embedding_image.c_str(), 1);
    ::setenv("EMBEDDING_MODEL_ID", c.model_id.c_str(), 1);
    ::setenv("INFINITY_IMAGE",     c.embedding_image.c_str(), 1);
    ::setenv("INFINITY_MODEL_ID",  c.model_id.c_str(), 1);
    ::setenv("QDRANT_COLLECTION",  kGauntletCollection.c_str(), 1);
    {
        std::ostringstream t;
        t << std::fixed << std::setprecision(2) << c.theta;
        ::setenv("DSCC_THETA", t.str().c_str(), 1);
    }
    ::setenv("DSCC_LOCK_HOLD_MS", std::to_string(c.lock_hold_ms).c_str(), 1);
}

void pull_all_models(const Config& c) {
    for (const auto& profile : kProfiles) {
        info("Pulling model: " + profile.model_id);
        std::ostringstream body;
        body << "{\"model\":\"" << escape_json(profile.model_id)
             << "\",\"stream\":false}";
        auto r = send_http_request(c.embedding_host, c.embedding_port,
                                   "POST", "/api/pull", body.str());
        if (r.status_code != 200)
            fail("model pull failed for " + profile.model_id +
                 " (status " + std::to_string(r.status_code) + ")");
        info("  ✓ " + profile.model_id + " ready");
    }
}

void start_full_stack(const Config& c) {
    set_stack_environment(c);
    info("Setting DSCC_THETA=" + format_float(c.theta, 2) +
         " DSCC_LOCK_HOLD_MS=" + std::to_string(c.lock_hold_ms) +
         " QDRANT_COLLECTION=" + kGauntletCollection);
    run_shell_or_throw(
        compose_command(c, "up -d --build qdrant embedding-service "
                           "dscc-node-1 dscc-node-2 dscc-node-3 dscc-node-4 dscc-node-5 dscc-proxy"),
        "docker compose up failed");
    wait_for_http_service("Qdrant", c.qdrant_host, c.qdrant_port, {"/collections", "/"});
    wait_for_embedding_service(c);
    info("Pre-pulling all 3 embedding models into Ollama...");
    pull_all_models(c);
    wait_for_leader(c, std::chrono::seconds(15));
    wait_for_dscc_service(c);
    info("Full stack is live — theta=" + format_float(c.theta, 2) +
         "  (all models pre-pulled)");
}

// Send a probe embedding request to the current config.model_id and return
// the vector dimension.  Logs the model name and dimension so the operator
// can confirm the correct model is active.
size_t verify_model_active(const Config& c) {
    const std::string probe = "embedding model verification probe";
    std::ostringstream body;
    body << "{\"model\":\"" << escape_json(c.model_id)
         << "\",\"input\":\"" << escape_json(probe) << "\"}";
    auto r = send_http_request(c.embedding_host, c.embedding_port,
                               "POST", "/v1/embeddings", body.str());
    if (r.status_code != 200)
        fail("model probe failed for " + c.model_id +
             " (status " + std::to_string(r.status_code) + ")");
    auto vec = parse_embedding_array(r.body);
    info("  Model probe: " + c.model_id +
         " → dim=" + std::to_string(vec.size()));
    return vec.size();
}

void stop_full_stack(const Config& c) {
    info("Tearing down full stack");
    auto r = run_shell_capture(compose_command(c, "down"));
    if (r.exit_code != 0) warn("docker compose down returned non-zero");
}

// ---------------------------------------------------------------------------
// Template loading — only A.json and D.json payloads needed for the gauntlet
// ---------------------------------------------------------------------------

struct GauntletTemplates {
    std::vector<TemplateDocument> all;
    std::vector<const TemplateDocument*> concept_a_all;
    const TemplateDocument* concept_a = nullptr;
    const TemplateDocument* concept_b = nullptr;
    size_t embedding_dim = 0;
};

GauntletTemplates load_gauntlet_templates(const Config& c) {
    GauntletTemplates gt;
    std::set<std::string> seen;
    int counter = 0;

    for (char label : {'A', 'D'}) {
        fs::path jp = fs::path(c.input_dir) / (std::string(1, label) + ".json");
        if (!fs::exists(jp)) fail("missing " + jp.string());
        std::string json = read_text_file(jp);
        auto entries = parse_payload_schedule_entries(json);
        if (entries.empty()) entries.push_back(json);
        for (auto& entry : entries) {
            std::string payload = parse_json_string_field(entry, "payload");
            if (!seen.insert(payload).second) continue;
            TemplateDocument doc;
            doc.template_id = "tpl-" + std::to_string(++counter);
            doc.source_file = jp.filename().string();
            doc.text = payload;
            doc.embedding = request_embedding(c, doc.text);
            gt.all.push_back(std::move(doc));
        }
    }

    for (auto& d : gt.all) {
        if (d.source_file == "A.json") {
            gt.concept_a_all.push_back(&d);
            if (!gt.concept_a) gt.concept_a = &d;
        }
        if (d.source_file == "D.json" && !gt.concept_b) gt.concept_b = &d;
    }
    if (!gt.concept_a) fail("no A.json templates found");
    if (!gt.concept_b) fail("no D.json templates found");
    gt.embedding_dim = gt.all.front().embedding.size();
    return gt;
}

// ---------------------------------------------------------------------------
// Workload construction — mirrors cases 11 and 12 from benchmark_runner.cpp
// ---------------------------------------------------------------------------

std::vector<BenchmarkOperation> build_paraphrase_workload(const GauntletTemplates& gt) {
    // Case 11: 10 writers cycling through all A.json paraphrase variants.
    std::vector<BenchmarkOperation> ops;
    const size_t vc = gt.concept_a_all.size();
    for (int i = 0; i < 10; ++i) {
        const TemplateDocument& tmpl = *gt.concept_a_all[static_cast<size_t>(i) % vc];
        BenchmarkOperation op;
        op.agent_id = "paraphrase_agent_" + std::to_string(i + 1);
        op.template_id = tmpl.template_id;
        op.text = tmpl.text;
        op.embedding = tmpl.embedding;
        op.operation = OperationType::kWrite;
        op.scheduled_offset_ms = 0;
        ops.push_back(std::move(op));
    }
    return ops;
}

std::vector<BenchmarkOperation> build_cross_domain_workload(const GauntletTemplates& gt) {
    // Case 12: first 6 on concept_a, last 6 on concept_b — burst.
    std::vector<BenchmarkOperation> ops;
    for (int i = 0; i < 12; ++i) {
        const TemplateDocument& tmpl = (i < 6) ? *gt.concept_a : *gt.concept_b;
        std::string prefix = (i < 6) ? "sustainability_agent_" : "construction_agent_";
        int idx = (i < 6) ? (i + 1) : (i - 5);
        BenchmarkOperation op;
        op.agent_id = prefix + std::to_string(idx);
        op.template_id = tmpl.template_id;
        op.text = tmpl.text;
        op.embedding = tmpl.embedding;
        op.operation = OperationType::kWrite;
        op.scheduled_offset_ms = 0;
        ops.push_back(std::move(op));
    }
    return ops;
}

// ---------------------------------------------------------------------------
// Traffic execution — fires burst of gRPC AcquireGuard calls
// ---------------------------------------------------------------------------

std::vector<OperationResult> run_scenario_traffic(const Config& c,
                                                  const std::string& collection,
                                                  const std::vector<BenchmarkOperation>& workload) {
    reset_qdrant_collection(c, collection);
    create_qdrant_collection(c, collection, workload.front().embedding.size());

    auto channel = grpc::CreateChannel(c.dscc_target, grpc::InsecureChannelCredentials());
    auto start = SteadyClock::now();
    auto now_ms = [&]() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            SteadyClock::now() - start).count();
    };
    int64_t unix_base = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<OperationResult> results(workload.size());
    std::vector<std::thread> threads;
    threads.reserve(workload.size());

    for (size_t i = 0; i < workload.size(); ++i) {
        threads.emplace_back([&, i]() {
            auto stub = dscc::LockService::NewStub(channel);
            auto& op = workload[i];
            results[i].operation = op;
            std::this_thread::sleep_until(start + std::chrono::milliseconds(op.scheduled_offset_ms));
            results[i].submit_ms = now_ms();

            dscc::AcquireRequest req;
            req.set_agent_id(op.agent_id);
            req.set_payload_text(op.text);
            req.set_source_file(op.template_id);
            req.set_timestamp_unix_ms(unix_base + op.scheduled_offset_ms);
            req.set_operation_type(op.operation == OperationType::kRead
                ? dscc::AcquireRequest::OPERATION_TYPE_READ
                : dscc::AcquireRequest::OPERATION_TYPE_WRITE);
            for (float v : op.embedding) req.add_embedding(v);

            grpc::ClientContext ctx;
            results[i].status = stub->AcquireGuard(&ctx, req, &results[i].response);
            results[i].finish_ms = now_ms();
            results[i].elapsed_ms = results[i].finish_ms - results[i].submit_ms;
        });
    }
    for (auto& t : threads) t.join();

    int expected_writes = static_cast<int>(std::count_if(workload.begin(), workload.end(),
        [](const BenchmarkOperation& o) { return o.operation == OperationType::kWrite; }));
    int actual = qdrant_point_count(c, collection);
    if (actual != expected_writes) {
        std::ostringstream msg;
        msg << "Qdrant count mismatch: expected=" << expected_writes << " actual=" << actual;
        fail(msg.str());
    }
    return results;
}

// ---------------------------------------------------------------------------
// Confusion matrix construction
//
// We iterate over all operation pairs (i,j) from both the Paraphrase Gauntlet
// and Cross-Domain Flood scenarios.  For each granted pair:
//
//   1. Compute cosine similarity between their embeddings.
//   2. Determine ground truth:  similarity >= θ → "should conflict" (positive)
//                                similarity <  θ → "should not conflict" (negative)
//   3. Determine prediction:    lock intervals do NOT overlap → system serialised
//                                  them → "predicted conflict" (positive prediction)
//                                lock intervals DO overlap → system allowed
//                                  concurrency → "predicted no conflict" (negative prediction)
//   4. Assign to the appropriate cell:
//        TP = should conflict     AND serialised
//        FN = should conflict     AND ran in parallel  (serialisation violation)
//        FP = should not conflict AND serialised       (unnecessary blocking)
//        TN = should not conflict AND ran in parallel  (correct concurrency)
// ---------------------------------------------------------------------------

void analyze_pairs(const std::string& scenario_label,
                   const std::vector<OperationResult>& ops,
                   float theta,
                   ConfusionMatrix& cm,
                   std::vector<PairOutcome>& pairs) {
    for (size_t i = 0; i < ops.size(); ++i) {
        for (size_t j = i + 1; j < ops.size(); ++j) {
            if (!ops[i].status.ok() || !ops[j].status.ok() ||
                !ops[i].response.granted() || !ops[j].response.granted()) continue;

            float sim = cosine_similarity(ops[i].operation.embedding, ops[j].operation.embedding);
            bool expected_conflict = (sim >= theta);
            bool overlap = intervals_overlap(
                ops[i].response.lock_acquired_unix_ms(), ops[i].response.lock_released_unix_ms(),
                ops[j].response.lock_acquired_unix_ms(), ops[j].response.lock_released_unix_ms());
            bool observed_serialized = !overlap;

            if (expected_conflict && observed_serialized)       ++cm.tp;
            else if (expected_conflict && !observed_serialized) ++cm.fn;
            else if (!expected_conflict && observed_serialized) ++cm.fp;
            else                                                ++cm.tn;

            PairOutcome po;
            po.scenario = scenario_label;
            po.text_a = ops[i].operation.text;
            po.text_b = ops[j].operation.text;
            po.agent_a = ops[i].operation.agent_id;
            po.agent_b = ops[j].operation.agent_id;
            po.cosine_similarity = sim;
            po.expected_conflict = expected_conflict;
            po.observed_serialized = observed_serialized;
            po.pass = (expected_conflict == observed_serialized);
            pairs.push_back(std::move(po));
        }
    }
}

ClassificationMetrics compute_classification_metrics(const ConfusionMatrix& cm) {
    ClassificationMetrics m;
    int total = cm.tp + cm.tn + cm.fp + cm.fn;
    m.accuracy  = total > 0 ? double(cm.tp + cm.tn) / double(total) : 0.0;
    m.precision = (cm.tp + cm.fp) > 0 ? double(cm.tp) / double(cm.tp + cm.fp) : 0.0;
    m.recall    = (cm.tp + cm.fn) > 0 ? double(cm.tp) / double(cm.tp + cm.fn) : 0.0;
    m.f1 = (m.precision + m.recall) > 0.0
        ? 2.0 * m.precision * m.recall / (m.precision + m.recall) : 0.0;
    m.serialization_score       = m.recall;
    m.distinct_parallelism_rate = (cm.tn + cm.fp) > 0 ? double(cm.tn) / double(cm.tn + cm.fp) : 0.0;
    m.false_positive_rate       = (cm.fp + cm.tn) > 0 ? double(cm.fp) / double(cm.fp + cm.tn) : 0.0;
    return m;
}

// ---------------------------------------------------------------------------
// Auto-generated conclusion
// ---------------------------------------------------------------------------

std::string generate_conclusion(const std::vector<ModelRunResult>& runs, float theta) {
    const ModelRunResult* best = nullptr;
    for (auto& r : runs) {
        if (!r.success) continue;
        if (!best || r.metrics.f1 > best->metrics.f1) best = &r;
    }
    if (!best) return "All model runs failed; no comparative conclusion can be drawn.";

    std::ostringstream s;
    s << best->model_id << " achieved the highest F1 of "
      << format_float(best->metrics.f1, 3)
      << " at theta=" << format_float(theta, 2)
      << ", correctly serializing "
      << format_float(best->metrics.recall * 100.0, 1)
      << "% of paraphrase pairs while allowing "
      << format_float(best->metrics.distinct_parallelism_rate * 100.0, 1)
      << "% of cross-domain pairs to run in parallel.";
    return s.str();
}

// ---------------------------------------------------------------------------
// JSON output — written atomically at the end
// ---------------------------------------------------------------------------

void write_pair_json(std::ostream& out, const PairOutcome& p, bool last) {
    out << "            {\n";
    out << "              \"scenario\": \"" << escape_json(p.scenario) << "\",\n";
    out << "              \"text_a\": \"" << escape_json(p.text_a) << "\",\n";
    out << "              \"text_b\": \"" << escape_json(p.text_b) << "\",\n";
    out << "              \"agent_a\": \"" << escape_json(p.agent_a) << "\",\n";
    out << "              \"agent_b\": \"" << escape_json(p.agent_b) << "\",\n";
    out << "              \"cosine_similarity\": " << std::fixed << std::setprecision(6) << p.cosine_similarity << ",\n";
    out << "              \"expected_outcome\": \"" << (p.expected_conflict ? "conflict" : "no_conflict") << "\",\n";
    out << "              \"observed_outcome\": \"" << (p.observed_serialized ? "serialized" : "parallel") << "\",\n";
    out << "              \"pass\": " << (p.pass ? "true" : "false") << "\n";
    out << "            }" << (last ? "\n" : ",\n");
}

void write_model_section(std::ostream& out, const ModelRunResult& r, bool last) {
    out << "    {\n";
    out << "      \"profile\": \"" << escape_json(r.profile_name) << "\",\n";
    out << "      \"model_id\": \"" << escape_json(r.model_id) << "\",\n";
    out << "      \"success\": " << (r.success ? "true" : "false") << ",\n";

    if (!r.success) {
        out << "      \"error\": \"" << escape_json(r.error_message) << "\"\n";
        out << "    }" << (last ? "\n" : ",\n");
        return;
    }

    out << "      \"embedding_dim\": " << r.embedding_dim << ",\n";
    out << "      \"paraphrase_ops\": " << r.paraphrase_ops << ",\n";
    out << "      \"cross_domain_ops\": " << r.cross_domain_ops << ",\n";
    out << "      \"total_pairs_analyzed\": " << r.pair_details.size() << ",\n";
    out << "      \"confusion_matrix\": {\n";
    out << "        \"TP\": " << r.matrix.tp << ",\n";
    out << "        \"FN\": " << r.matrix.fn << ",\n";
    out << "        \"FP\": " << r.matrix.fp << ",\n";
    out << "        \"TN\": " << r.matrix.tn << "\n";
    out << "      },\n";
    out << "      \"classification_metrics\": {\n";
    out << "        \"accuracy\": " << format_float(r.metrics.accuracy, 3) << ",\n";
    out << "        \"precision\": " << format_float(r.metrics.precision, 3) << ",\n";
    out << "        \"recall\": " << format_float(r.metrics.recall, 3) << ",\n";
    out << "        \"f1_score\": " << format_float(r.metrics.f1, 3) << ",\n";
    out << "        \"serialization_score\": " << format_float(r.metrics.serialization_score, 3) << ",\n";
    out << "        \"distinct_parallelism_rate\": " << format_float(r.metrics.distinct_parallelism_rate, 3) << ",\n";
    out << "        \"false_positive_rate\": " << format_float(r.metrics.false_positive_rate, 3) << "\n";
    out << "      },\n";
    out << "      \"pair_details\": [\n";
    for (size_t i = 0; i < r.pair_details.size(); ++i) {
        write_pair_json(out, r.pair_details[i], i + 1 == r.pair_details.size());
    }
    out << "      ]\n";
    out << "    }" << (last ? "\n" : ",\n");
}

void write_summary_table(std::ostream& out, const std::vector<ModelRunResult>& runs) {
    out << "  \"summary_comparison\": [\n";
    for (size_t i = 0; i < runs.size(); ++i) {
        auto& r = runs[i];
        out << "    {\"model\": \"" << escape_json(r.model_id) << "\"";
        if (r.success) {
            out << ", \"embedding_dim\": " << r.embedding_dim
                << ", \"TP\": " << r.matrix.tp
                << ", \"FN\": " << r.matrix.fn
                << ", \"FP\": " << r.matrix.fp
                << ", \"TN\": " << r.matrix.tn
                << ", \"accuracy\": " << format_float(r.metrics.accuracy, 3)
                << ", \"precision\": " << format_float(r.metrics.precision, 3)
                << ", \"recall\": " << format_float(r.metrics.recall, 3)
                << ", \"f1_score\": " << format_float(r.metrics.f1, 3)
                << ", \"serialization_score\": " << format_float(r.metrics.serialization_score, 3)
                << ", \"distinct_parallelism_rate\": " << format_float(r.metrics.distinct_parallelism_rate, 3)
                << ", \"false_positive_rate\": " << format_float(r.metrics.false_positive_rate, 3);
        } else {
            out << ", \"error\": \"" << escape_json(r.error_message) << "\"";
        }
        out << "}" << (i + 1 == runs.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
}

void write_results_json(const std::string& path, const std::vector<ModelRunResult>& runs,
                        const Config& c) {
    fs::path fp(path);
    if (!fp.parent_path().empty()) fs::create_directories(fp.parent_path());

    // Build entire JSON in memory, then write atomically.
    std::ostringstream buf;

    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    int total_pairs = 0;
    for (auto& r : runs) total_pairs += static_cast<int>(r.pair_details.size());

    buf << "{\n";
    buf << "  \"demo\": \"paraphrase_gauntlet_matrix\",\n";
    buf << "  \"timestamp_unix_s\": " << ts << ",\n";
    buf << "  \"theta\": " << format_float(c.theta, 2) << ",\n";
    buf << "  \"models_tested\": " << runs.size() << ",\n";
    buf << "  \"paraphrase_ops_per_model\": 10,\n";
    buf << "  \"cross_domain_ops_per_model\": 12,\n";
    buf << "  \"docker_compose\": \"docker-compose.yml\",\n";
    buf << "  \"project_root\": \"" << escape_json(c.project_root) << "\",\n";

    write_summary_table(buf, runs);

    buf << "  \"conclusion\": \"" << escape_json(generate_conclusion(runs, c.theta)) << "\",\n";

    buf << "  \"model_runs\": [\n";
    for (size_t i = 0; i < runs.size(); ++i) {
        write_model_section(buf, runs[i], i + 1 == runs.size());
    }
    buf << "  ]\n";
    buf << "}\n";

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) fail("could not write results to " + path);
    out << buf.str();
    out.flush();
}

// ---------------------------------------------------------------------------
// Console summary
// ---------------------------------------------------------------------------

void print_console_summary(const std::vector<ModelRunResult>& runs, float theta) {
    static constexpr const char* kDiv =
        "================================================================================";

    std::cout << "\n" << kDiv << "\n";
    std::cout << "\033[1m\033[36m  PARAPHRASE GAUNTLET × MODEL MATRIX — CONFUSION MATRIX RESULTS\033[0m\n";
    std::cout << "  θ = " << format_float(theta, 2) << "\n";
    std::cout << kDiv << "\n\n";

    std::cout << std::left << std::setw(28) << "Model"
              << std::right
              << std::setw(5) << "Dim"
              << std::setw(5) << "TP" << std::setw(5) << "FN"
              << std::setw(5) << "FP" << std::setw(5) << "TN"
              << std::setw(10) << "Acc"
              << std::setw(10) << "Prec"
              << std::setw(10) << "Recall"
              << std::setw(10) << "F1"
              << std::setw(10) << "FPR"
              << "\n";
    std::cout << std::string(102, '-') << "\n";

    for (auto& r : runs) {
        std::cout << std::left << std::setw(28) << r.model_id;
        if (r.success) {
            std::cout << std::right
                      << std::setw(5) << r.embedding_dim
                      << std::setw(5) << r.matrix.tp
                      << std::setw(5) << r.matrix.fn
                      << std::setw(5) << r.matrix.fp
                      << std::setw(5) << r.matrix.tn
                      << std::setw(10) << format_float(r.metrics.accuracy, 3)
                      << std::setw(10) << format_float(r.metrics.precision, 3)
                      << std::setw(10) << format_float(r.metrics.recall, 3)
                      << std::setw(10) << format_float(r.metrics.f1, 3)
                      << std::setw(10) << format_float(r.metrics.false_positive_rate, 3);
        } else {
            std::cout << "  FAILED: " << r.error_message;
        }
        std::cout << "\n";
    }
    std::cout << "\n" << generate_conclusion(runs, theta) << "\n";
    std::cout << kDiv << "\n";
}

// ---------------------------------------------------------------------------
// Default output path
// ---------------------------------------------------------------------------

std::string default_output_path(const Config& c) {
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (fs::path(c.project_root) / "logs" /
            ("paraphrase_gauntlet_results_" + std::to_string(ts) + ".json")).string();
}

}  // namespace

// ===========================================================================
// Entry point
//
// Lifecycle:
//   1. Start the FULL stack once (proxy, 5 nodes, Qdrant, embedding-service).
//      Theta and lock_hold_ms are baked into the nodes via env vars.
//      All three embedding models are pre-pulled into Ollama.
//   2. For each embedding model (no container restarts — model selected per-request):
//      a. Probe-verify the model produces the expected embedding dimension.
//      b. Re-embed the template corpus with the new model.
//      c. Flush the Qdrant collection, run the Paraphrase Gauntlet.
//      d. Flush again, run the Cross-Domain Flood.
//      e. Build the confusion matrix from both phases.
//   3. Tear down the full stack (if E2E_TEARDOWN=1).
//   4. Write the results JSON atomically.
// ===========================================================================

int main() {
    Config config;
    bool stack_started = false;

    try {
        config = load_config();
        std::string output_path = config.output_path.empty()
            ? default_output_path(config) : config.output_path;

        info("Paraphrase Gauntlet x Model Matrix Demo");
        info("theta=" + format_float(config.theta, 2) +
             "  lock_hold_ms=" + std::to_string(config.lock_hold_ms) +
             "  models=" + std::to_string(kProfiles.size()) +
             "  output=" + output_path);

        // ---- Start full stack once with the first model ----
        config.embedding_profile = kProfiles[0].name;
        config.model_id          = kProfiles[0].model_id;
        config.embedding_image   = kProfiles[0].image;

        start_full_stack(config);
        stack_started = true;

        std::vector<ModelRunResult> all_runs;
        all_runs.reserve(kProfiles.size());

        for (size_t pi = 0; pi < kProfiles.size(); ++pi) {
            const EmbeddingProfile& profile = kProfiles[pi];
            ModelRunResult run;
            run.profile_name = profile.name;
            run.model_id     = profile.model_id;

            config.embedding_profile = profile.name;
            config.model_id          = profile.model_id;
            config.embedding_image   = profile.image;

            info("====== Model: " + profile.model_id + " (" + profile.name + ") ======");

            try {
                // ---- Verify model is responding (no container restart needed) ----
                // Ollama serves all 3 models from a single container; the model is
                // selected per-request via the "model" field in the API body.
                // A probe request confirms the expected model is active and logs the
                // embedding dimension as proof (all-minilm=384, bge-m3=1024, qwen3=1024).
                size_t probe_dim = verify_model_active(config);

                // ---- Load templates via embedding API (stack is already live) ----
                info("Loading A.json + D.json templates with " + profile.model_id + "...");
                GauntletTemplates gt = load_gauntlet_templates(config);
                run.embedding_dim = gt.embedding_dim;
                info("Templates loaded: " + std::to_string(gt.all.size()) +
                     " unique texts (" + std::to_string(gt.concept_a_all.size()) +
                     " A-variants, concept_b from D.json)"
                     " — embedding dim=" + std::to_string(gt.embedding_dim));
                if (gt.embedding_dim != probe_dim) {
                    fail("embedding dimension mismatch: probe=" +
                         std::to_string(probe_dim) + " templates=" +
                         std::to_string(gt.embedding_dim));
                }

                // ---- Phase 1: Paraphrase Gauntlet (positive cases) ----
                info("Phase 1: Paraphrase Gauntlet (10 ops, burst, theta=" +
                     format_float(config.theta, 2) + ")");
                auto para_workload = build_paraphrase_workload(gt);
                auto para_results  = run_scenario_traffic(config, kGauntletCollection,
                                                          para_workload);
                run.paraphrase_ops = static_cast<int>(para_results.size());

                // ---- Phase 2: Cross-Domain Flood (negative cases) ----
                info("Phase 2: Cross-Domain Flood (12 ops, burst, theta=" +
                     format_float(config.theta, 2) + ")");
                auto xdom_workload = build_cross_domain_workload(gt);
                auto xdom_results  = run_scenario_traffic(config, kGauntletCollection,
                                                          xdom_workload);
                run.cross_domain_ops = static_cast<int>(xdom_results.size());

                // ---- Analyze: build confusion matrix from both phases ----
                analyze_pairs("paraphrase_gauntlet", para_results, config.theta,
                              run.matrix, run.pair_details);
                analyze_pairs("cross_domain_flood", xdom_results, config.theta,
                              run.matrix, run.pair_details);
                run.metrics = compute_classification_metrics(run.matrix);
                run.success = true;

                info("Model " + profile.model_id + " complete — " +
                     "TP=" + std::to_string(run.matrix.tp) +
                     " FN=" + std::to_string(run.matrix.fn) +
                     " FP=" + std::to_string(run.matrix.fp) +
                     " TN=" + std::to_string(run.matrix.tn) +
                     "  F1=" + format_float(run.metrics.f1, 3));

            } catch (const std::exception& ex) {
                run.success = false;
                run.error_message = ex.what();
                warn("Model " + profile.model_id + " FAILED: " + run.error_message);
            }

            all_runs.push_back(std::move(run));
        }

        // ---- Tear down full stack ----
        if (config.teardown_on_exit) {
            stop_full_stack(config);
            stack_started = false;
        }

        // ---- Write atomic results file ----
        write_results_json(output_path, all_runs, config);
        info("Results written to " + output_path);

        print_console_summary(all_runs, config.theta);

        return 0;

    } catch (const std::exception& ex) {
        if (stack_started && config.teardown_on_exit) stop_full_stack(config);
        std::cerr << "[ERROR] " << ex.what() << std::endl;
        return 1;
    }
}
