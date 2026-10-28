// Tamper-evident audit ledger: a hash chain stored as JSON lines.
//   hash(entry) = SHA-256( every field of the entry + hash of the previous entry )
// Edit, delete or reorder any entry and every hash after it stops matching.
#pragma once
#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"
#include "sha256.hpp"

namespace bg {

inline const std::string GENESIS_HASH(64, '0');

struct Entry {
    long index = 0;
    std::string timestamp, request_id, prompt, decision, reason, output, prev_hash, hash;
};

struct Verification {
    bool valid = true;
    size_t length = 0;
    long broken_at = -1;
    std::string detail, head;
};

// Length-prefixed fields so "ab"+"c" can never collide with "a"+"bc".
inline std::string canonical(const Entry& e) {
    auto f = [](const std::string& s) { return std::to_string(s.size()) + ":" + s + ";"; };
    return f(std::to_string(e.index)) + f(e.timestamp) + f(e.request_id) + f(e.prompt) + f(e.decision) +
           f(e.reason) + f(e.output) + f(e.prev_hash);
}
inline std::string compute_hash(const Entry& e) { return sha256_hex(canonical(e)); }

inline std::string now_iso_utc() {
    using namespace std::chrono;
    auto tp = system_clock::now();
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()).count() % 1000;
    std::time_t t = system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    char out[64];
    std::snprintf(out, sizeof out, "%s.%03ld+00:00", buf, static_cast<long>(ms));
    return out;
}

inline std::string entry_json(const Entry& e) {
    auto s = [](const std::string& k, const std::string& v) { return "\"" + k + "\":\"" + json_escape(v) + "\""; };
    return "{\"index\":" + std::to_string(e.index) + "," + s("timestamp", e.timestamp) + "," +
           s("request_id", e.request_id) + "," + s("prompt", e.prompt) + "," + s("decision", e.decision) + "," +
           s("reason", e.reason) + "," + s("output", e.output) + "," + s("prev_hash", e.prev_hash) + "," +
           s("hash", e.hash) + "}";
}

inline std::string verification_json(const Verification& v) {
    return std::string("{\"valid\":") + (v.valid ? "true" : "false") + ",\"length\":" + std::to_string(v.length) +
           ",\"broken_at\":" + (v.broken_at < 0 ? "null" : std::to_string(v.broken_at)) + ",\"detail\":\"" +
           json_escape(v.detail) + "\",\"head\":" + (v.valid ? "\"" + v.head + "\"" : "null") + "}";
}

class Ledger {
public:
    explicit Ledger(std::string path) : path_(std::move(path)) {
        std::ifstream in(path_);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            JObj o;
            if (!json_parse_flat(line, o)) continue;
            Entry e;
            e.index = std::stol(jget(o, "index", "0"));
            e.timestamp = jget(o, "timestamp");
            e.request_id = jget(o, "request_id");
            e.prompt = jget(o, "prompt");
            e.decision = jget(o, "decision");
            e.reason = jget(o, "reason");
            e.output = jget(o, "output");
            e.prev_hash = jget(o, "prev_hash");
            e.hash = jget(o, "hash");
            entries_.push_back(e);
        }
    }

    Entry append(const std::string& request_id, const std::string& prompt, const std::string& decision,
                 const std::string& reason, const std::string& output) {
        std::lock_guard<std::mutex> lock(mu_);
        Entry e;
        e.index = static_cast<long>(entries_.size());
        e.timestamp = now_iso_utc();
        e.request_id = request_id;
        e.prompt = prompt;
        e.decision = decision;
        e.reason = reason;
        e.output = output;
        e.prev_hash = entries_.empty() ? GENESIS_HASH : entries_.back().hash;
        e.hash = compute_hash(e);
        entries_.push_back(e);
        std::ofstream out(path_, std::ios::app);
        out << entry_json(e) << "\n";
        return e;
    }

    std::vector<Entry> entries() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_;
    }

    Verification verify() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::string prev = GENESIS_HASH;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (e.index != static_cast<long>(i)) return bad(i, "index out of sequence");
            if (e.prev_hash != prev) return bad(i, "prev_hash does not match previous entry");
            if (compute_hash(e) != e.hash) return bad(i, "entry contents do not match its hash");
            prev = e.hash;
        }
        Verification v;
        v.length = entries_.size();
        v.detail = "all hashes verified";
        v.head = prev;
        return v;
    }

    // Demo helper: edit one record WITHOUT re-hashing, to show that verify() catches it.
    bool tamper(long index) {
        std::lock_guard<std::mutex> lock(mu_);
        if (index < 0 || index >= static_cast<long>(entries_.size())) return false;
        Entry& e = entries_[static_cast<size_t>(index)];
        e.output = "[edited after the fact]";
        e.decision = (e.decision == "allowed") ? "blocked" : "allowed";
        rewrite();
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.clear();
        rewrite();
    }

private:
    Verification bad(size_t i, const std::string& why) const {
        Verification v;
        v.valid = false;
        v.length = entries_.size();
        v.broken_at = static_cast<long>(i);
        v.detail = why;
        return v;
    }
    void rewrite() {
        std::ofstream out(path_, std::ios::trunc);
        for (const Entry& e : entries_) out << entry_json(e) << "\n";
    }
    std::string path_;
    std::vector<Entry> entries_;
    mutable std::mutex mu_;
};

}  // namespace bg
