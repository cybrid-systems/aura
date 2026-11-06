// evo_log_4910.hh — Issue #4910: user-visible evolution log (worldline).
//
// One JSON object per line, appended to the file named by AURA_EVO_LOG
// (`aura run` in a project defaults it to <root>/.aura/evo/log.jsonl).
// Unset → every entry point is a single relaxed load + branch.
//
// Record schema (v=1):
//   {"v":1,"id","parent","run","seq","ts_ms","kind","target","decision",
//    "reason","evidence"(JSON value),"old","new","dur_ms"}
// kind: an op name (mutate:rebind, ast:restore, …) or "record" (evo:record!).
// decision: KEEP | ROLLBACK | user string. The worldline tip advances on KEEP;
// a candidate's parent is the tip when it was recorded (or the explicit
// parent evo:record! names), so `aura evo log` can draw the tree.
//
// The path is operator configuration (env), never program-chosen: the log is
// an audit trail like the security WAL, not a program file write.
#pragma once

#include "core/ownership.hh" // #4936 unique_file

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace aura::evo {

struct LogState {
    std::mutex mu;
    std::string path;
    std::string run;
    std::uint64_t seq = 0;
    std::string tip;
    std::string last_error;
};

inline LogState& log_state() noexcept {
    static LogState s;
    return s;
}

// -1 = not yet read from the environment, 0 = off, 1 = on.
inline std::atomic<int>& log_flag() noexcept {
    static std::atomic<int> f{-1};
    return f;
}

inline std::string default_run_id() {
    if (const char* r = std::getenv("AURA_EVO_RUN"); r && *r)
        return r;
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    return std::format("{:04}{:02}{:02}T{:02}{:02}{:02}Z-{}", tm.tm_year + 1900, tm.tm_mon + 1,
                       tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(::getpid()));
}

inline bool log_enabled() noexcept {
    int f = log_flag().load(std::memory_order_acquire);
    if (f >= 0) [[likely]]
        return f == 1;
    auto& s = log_state();
    std::scoped_lock lk(s.mu);
    f = log_flag().load(std::memory_order_relaxed);
    if (f < 0) {
        const char* p = std::getenv("AURA_EVO_LOG");
        s.path = (p && *p) ? p : "";
        if (!s.path.empty())
            s.run = default_run_id();
        f = s.path.empty() ? 0 : 1;
        log_flag().store(f, std::memory_order_release);
    }
    return f == 1;
}

inline std::string log_path() {
    if (!log_enabled())
        return {};
    auto& s = log_state();
    std::scoped_lock lk(s.mu);
    return s.path;
}

inline void json_escape_into(std::string& out, std::string_view in) {
    out.push_back('"');
    for (const unsigned char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

struct Entry {
    std::string_view kind;
    std::string_view target;
    std::string_view decision;
    std::string_view reason;
    std::string_view evidence_json; // raw JSON value; empty → null
    std::string_view old_src;
    std::string_view new_src;
    std::string_view parent; // empty → current worldline tip
    double dur_ms = 0.0;
};

// Appends one line; returns the new record id ("" when off or on I/O error).
// Cold audit write. A non-empty id is always run + '#' + sequence.
inline std::string log_append(const Entry& e)
    post(r: r.empty() || r.find('#') != std::string::npos) {
    if (!log_enabled())
        return {};
    auto& s = log_state();
    std::scoped_lock lk(s.mu);
    const std::uint64_t seq = ++s.seq;
    std::string id = s.run + "#" + std::to_string(seq);
    const std::string parent = e.parent.empty() ? s.tip : std::string(e.parent);
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    std::string line;
    line.reserve(256 + e.old_src.size() + e.new_src.size() + e.evidence_json.size());
    line += "{\"v\":1,\"id\":";
    json_escape_into(line, id);
    line += ",\"parent\":";
    json_escape_into(line, parent);
    line += ",\"run\":";
    json_escape_into(line, s.run);
    line += ",\"seq\":" + std::to_string(seq);
    line += ",\"ts_ms\":" + std::to_string(ts);
    line += ",\"kind\":";
    json_escape_into(line, e.kind);
    line += ",\"target\":";
    json_escape_into(line, e.target);
    line += ",\"decision\":";
    json_escape_into(line, e.decision);
    line += ",\"reason\":";
    json_escape_into(line, e.reason);
    line += ",\"evidence\":";
    line += e.evidence_json.empty() ? std::string_view("null") : e.evidence_json;
    line += ",\"old\":";
    json_escape_into(line, e.old_src);
    line += ",\"new\":";
    json_escape_into(line, e.new_src);
    line += ",\"dur_ms\":";
    line += std::format("{:.3f}", e.dur_ms);
    line += "}\n";
    std::error_code ec;
    const std::filesystem::path p(s.path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);
    ::aura::core::unique_file f = ::aura::core::open_file(s.path.c_str(), "a");
    if (!f) {
        s.last_error = "cannot open " + s.path;
        return {};
    }
    // #4936: the close is the final flush; a failure there (ENOSPC) used to
    // be ignored and the entry reported as written.
    const bool wrote = std::fwrite(line.data(), 1, line.size(), f.get()) == line.size();
    const bool ok = ::aura::core::close_file(f) && wrote;
    if (!ok) {
        s.last_error = "short write to " + s.path;
        return {};
    }
    if (e.decision == "KEEP")
        s.tip = id;
    return id;
}

// Tests only: re-read AURA_EVO_LOG on next use.
inline void log_reset_for_test() noexcept {
    auto& s = log_state();
    std::scoped_lock lk(s.mu);
    s.path.clear();
    s.run.clear();
    s.seq = 0;
    s.tip.clear();
    s.last_error.clear();
    log_flag().store(-1, std::memory_order_release);
}

} // namespace aura::evo
