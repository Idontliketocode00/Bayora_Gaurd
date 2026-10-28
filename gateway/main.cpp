// Blind intermediary gateway.
// Red team, blue team and the LLM never talk to each other: everything goes
// through here, and every caller-visible reply has the same shape and the same
// (bucketed) latency whether the request was allowed, blocked or failed.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#include "json.hpp"
#include "ledger.hpp"
#include "net.hpp"

using namespace bg;
using steady = std::chrono::steady_clock;

static std::string env(const char* k, const std::string& d) {
    const char* v = std::getenv(k);
    return v && *v ? v : d;
}

struct Config {
    int port = std::stoi(env("PORT", "8000"));
    std::string blue_host = env("BLUE_HOST", "blue");
    int blue_port = std::stoi(env("BLUE_PORT", "8001"));
    std::string llm_host = env("LLM_HOST", "llm");
    int llm_port = std::stoi(env("LLM_PORT", "11434"));
    std::string llm_model = env("LLM_MODEL", "tinyllama");
    bool mock_llm = env("MOCK_LLM", "0") == "1";
    // Fixed-deadline mode (default): EVERY request gets its reply at exactly `bucket` seconds.
    // If the LLM cannot answer within that budget the request fails closed at the same moment,
    // so latency never reveals whether a prompt was allowed, blocked or slow.
    bool fixed_deadline = env("FIXED_DEADLINE", "1") == "1";
    double bucket = std::stod(env("DELAY_BUCKET_SECONDS", mock_llm ? "3.0" : "15.0"));
    int llm_timeout = std::stoi(env("LLM_TIMEOUT_SECONDS", "25"));  // only used when FIXED_DEADLINE=0
    int llm_num_predict = std::stoi(env("LLM_NUM_PREDICT", "100"));  // cap reply length so a small model cannot ramble
    std::string ledger_path = env("LEDGER_PATH", "ledger.jsonl");
    std::string auditor_token = env("AUDITOR_TOKEN", "bayora-demo");
    std::string static_dir = env("STATIC_DIR", "static");
    bool demo_mode = env("DEMO_MODE", "1") == "1";
};

static const std::string REFUSAL = "I can't help with that request.";

struct Outcome {
    std::string decision, reason, output;
};

// Runs blue-team inspection, then the LLM. Fails closed on any error.
static Outcome run_pipeline(const Config& c, const std::string& prompt, steady::time_point start) {
    const std::string esc = json_escape(prompt);
    // Seconds left in this request's time budget (fixed-deadline mode), keeping a small reserve for logging.
    auto left = [&]() { return c.bucket - 0.2 - std::chrono::duration<double>(steady::now() - start).count(); };
    if (c.fixed_deadline && left() <= 0.05) return {"error", "no time budget left (fail-closed)", REFUSAL};

    auto br = http_post(c.blue_host, c.blue_port, "/inspect", "{\"prompt\":\"" + esc + "\"}",
                        c.fixed_deadline ? std::min(5.0, left()) : 5.0);
    if (!br || br->status != 200) return {"error", "blue team unreachable (fail-closed)", REFUSAL};
    JObj verdict;
    if (!json_parse_flat(br->body, verdict)) return {"error", "blue team sent invalid JSON (fail-closed)", REFUSAL};
    if (jget(verdict, "allow") != "true") {
        std::string reason = jget(verdict, "reason", "blocked by guardrail");
        std::string cat = jget(verdict, "category");
        if (cat == "null") cat.clear();
        return {"blocked", cat.empty() ? reason : reason + " [" + cat + "]", REFUSAL};
    }

    if (c.mock_llm)
        return {"allowed", "passed guardrails (mock LLM)", "[mock] You asked: " + prompt.substr(0, 120)};

    std::string body = "{\"model\":\"" + json_escape(c.llm_model) + "\",\"prompt\":\"" + esc + "\",\"stream\":false,\"options\":{\"num_predict\":" +
                       std::to_string(c.llm_num_predict) + "}}";
    if (c.fixed_deadline && left() <= 0.05) return {"error", "no time budget left for the LLM (fail-closed)", REFUSAL};
    auto lr = http_post(c.llm_host, c.llm_port, "/api/generate", body,
                        c.fixed_deadline ? left() : static_cast<double>(c.llm_timeout));
    if (!lr || lr->status != 200)
        return {"error", c.fixed_deadline ? "LLM did not answer within the time budget (fail-closed)"
                                          : "LLM unreachable (fail-closed)", REFUSAL};
    JObj reply;
    if (!json_parse_flat(lr->body, reply) || !reply.count("response"))
        return {"error", "LLM sent invalid JSON (fail-closed)", REFUSAL};
    std::string out = trim(reply["response"].v);
    return {"allowed", "passed guardrails", out.empty() ? "(no output)" : out};
}

static std::string new_request_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(rng()));
    return std::string(buf).substr(0, 12);
}

static bool authed(const Config& c, const Request& rq) {
    auto it = rq.headers.find("x-auditor-token");
    std::string given = it == rq.headers.end() ? "" : it->second;
    unsigned diff = static_cast<unsigned>(given.size() ^ c.auditor_token.size());  // constant-time compare
    for (size_t i = 0; i < given.size() && i < c.auditor_token.size(); ++i) diff |= (given[i] ^ c.auditor_token[i]);
    return diff == 0;
}

static Response json_resp(int status, const std::string& body) {
    Response r;
    r.status = status;
    r.body = body;
    return r;
}

int main() {
    Config c;
    Ledger ledger(c.ledger_path);
    std::cout << "[gateway] :" << c.port << "  blue=" << c.blue_host << ":" << c.blue_port << "  llm="
              << (c.mock_llm ? "MOCK" : c.llm_host + ":" + std::to_string(c.llm_port)) << "  bucket=" << c.bucket
              << "s (" << (c.fixed_deadline ? "fixed deadline" : "rounded buckets") << ")  ledger=" << c.ledger_path << " (" << ledger.entries().size() << " entries)" << std::endl;

    serve(c.port, [&](const Request& rq) -> Response {
        // ---------------- public: what the red team can reach ----------------
        if (rq.method == "GET" && rq.path == "/health") return json_resp(200, "{\"status\":\"ok\"}");

        if (rq.method == "POST" && rq.path == "/submit") {
            const auto start = steady::now();
            JObj o;
            if (!json_parse_flat(rq.body, o) || !o.count("prompt") || !o["prompt"].is_str)
                return json_resp(400, "{\"error\":\"expected {\\\"prompt\\\": \\\"...\\\"}\"}");
            const std::string prompt = o["prompt"].v;
            if (prompt.empty() || prompt.size() > 2000)
                return json_resp(400, "{\"error\":\"prompt must be 1-2000 bytes\"}");

            const std::string id = new_request_id();
            Outcome out = run_pipeline(c, prompt, start);
            ledger.append(id, prompt, out.decision, out.reason, out.output);

            // Pad to the next delay bucket (>= bucket) so latency reveals nothing.
            // sleep_until a FIXED point in time: unlike a fixed sleep, it is not
            // thrown off by how long the work above happened to take.
            const double elapsed = std::chrono::duration<double>(steady::now() - start).count();
            const double target = c.fixed_deadline ? std::max(c.bucket, elapsed)
                                                   : std::max(c.bucket, std::ceil(elapsed / c.bucket) * c.bucket);
            std::this_thread::sleep_until(start + std::chrono::duration_cast<steady::duration>(
                                                      std::chrono::duration<double>(target)));

            // Identical shape for allowed / blocked / error.
            return json_resp(200, "{\"status\":\"ok\",\"request_id\":\"" + id + "\",\"output\":\"" +
                                      json_escape(out.output) + "\"}");
        }

        // ---------------- auditor only (token required) ----------------
        if (rq.path == "/ledger" || rq.path == "/verify" || rq.path.rfind("/demo/", 0) == 0) {
            if (!authed(c, rq)) return json_resp(401, "{\"detail\":\"auditor token required\"}");

            if (rq.method == "GET" && rq.path == "/ledger") {
                std::string j = "{\"entries\":[";
                auto es = ledger.entries();
                for (size_t i = 0; i < es.size(); ++i) j += (i ? "," : "") + entry_json(es[i]);
                return json_resp(200, j + "],\"verification\":" + verification_json(ledger.verify()) + "}");
            }
            if (rq.method == "GET" && rq.path == "/verify")
                return json_resp(200, verification_json(ledger.verify()));

            if (c.demo_mode && rq.method == "POST" && rq.path == "/demo/reset") {
                ledger.reset();
                return json_resp(200, "{\"reset\":true}");
            }
            const std::string tp = "/demo/tamper/";
            if (c.demo_mode && rq.method == "POST" && rq.path.rfind(tp, 0) == 0) {
                long idx = -1;
                try { idx = std::stol(rq.path.substr(tp.size())); } catch (...) {}
                if (!ledger.tamper(idx)) return json_resp(404, "{\"detail\":\"no such entry\"}");
                return json_resp(200, "{\"tampered\":" + std::to_string(idx) + "}");
            }
            return json_resp(404, "{\"detail\":\"not found\"}");
        }

        // ---------------- dashboard page ----------------
        if (rq.method == "GET" && rq.path == "/") {
            // Works from the project root (gateway/static), inside Docker (static), or via STATIC_DIR.
            std::ifstream in;
            for (const std::string& dir : {c.static_dir, std::string("gateway/static"), std::string("static")}) {
                in.open(dir + "/index.html", std::ios::binary);
                if (in) break;
                in.clear();
            }
            if (!in) return json_resp(404, "{\"detail\":\"dashboard not found: run from the project folder or set STATIC_DIR\"}");
            std::stringstream ss;
            ss << in.rdbuf();
            Response r;
            r.content_type = "text/html; charset=utf-8";
            r.body = ss.str();
            return r;
        }
        return json_resp(404, "{\"detail\":\"not found\"}");
    });
}
