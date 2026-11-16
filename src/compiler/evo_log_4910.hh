// evo_log_4910.hh — Issue #4910: user-visible evolution log (worldline).
//
// One JSON object per line, appended to the file named by AURA_EVO_LOG
// (`aura run` in a project defaults it to <root>/.aura/evo/log.jsonl).
// Unset → every entry point is a single relaxed load + branch.
//
// Record schema (v=1):
//   {"v":1,"id","parent","run","seq","ts_ms","kind","target","decision",
//    "reason","evidence"(JSON value),"old","new","dur_ms"}
// kind: an op name (mutate :rebind, ast:restore, …) or "record" (evo:record!).
//
// Schema v=2 (#4939) adds optional members after dur_ms — multi-objective,
// constrained, multi-parent and behavior-description records:
//   {"v":2,…,"dur_ms":D,"algo":S,"gen":N,"slot":N,"search":S,"candidate":S,
//    "parents":[S…],"behavior":J,"objectives":J,"constraints":J}
// Only the members the caller supplies are written, always in that fixed
// order (never hash iteration order) so identical runs stay byte-identical.
// Readers must accept v=1 and v=2 in the same file: the JSONL shape is
// additive and "aura evo log --json" echoes each line verbatim.
// decision: KEEP | ROLLBACK | FORK | user string. The worldline tip advances
// on a kind == "record" KEEP only (#5139: an automatic mutate:rebind /
// mw:restore reuses "KEEP" but is not a candidate record). FORK (#5088) is a mutate:* that
// succeeded inside a child workspace: the rewrite is discarded with that workspace (and with a
// trial subprocess), so it must not be counted as a live KEEP. Those lines are schema v=2 and carry
// "scope":"fork" after dur_ms. A ROLLBACK in the same child workspace keeps decision ROLLBACK and
// the same scope field. A candidate record's parent is the tip when it was recorded (or the
// explicit parent evo:record! names), so `aura evo log` can draw the tree.
// The tip is the previous RECORD's id: an automatic mutate:rebind / mw:restore
// row never becomes the parent a later record inherits (#5139).
//
// The path is operator configuration (env), never program-chosen: the log is
// an audit trail like the security WAL, not a program file write.
//
// Issue #5251 (v0.2.6): the trace is ON by default in process. Every record
// also lands in a bounded ring (AURA_EVO_RING lines, default 4096; 0 = off),
// so a single-file run and a serve worker can read a rebind without
// AURA_EVO_LOG. The JSONL file is optional spill, written only when
// AURA_EVO_LOG names a path. (query "qr:evo-trace") reads the ring; the one
// filtered entry is std/evo's (evo:trace opts).
// Additive members, never reordered: "gate" / "weight" slots for the unified
// acceptance gate (#5255), and "mutation" (the workspace mutation-log id a
// mutate:* row recorded) plus "link" (the trace id that row came from) on the
// per-mutation ROLLBACK a restore writes.
#pragma once

#include "core/runtime_config.hh"
#include "compiler/json_escape.hh" // #5234: shared RFC 8259 escaper
#include "core/ownership.hh"       // #4936 unique_file

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <format>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace aura::evo {

// Issue #5251: the ring is bounded by records AND by bytes. A rebind row
// carries the old and new source, so a count-only bound could hold gigabytes
// of define bodies. A single line larger than the budget is kept out of the
// ring (the file, when configured, still has it).
inline constexpr std::size_t kTraceRingBytes = 8u << 20;

// Issue #5251: identity of one ring entry, enough to answer "which trace row
// recorded mutation N, and what did it name" without re-parsing the line.
struct TraceMeta {
    std::string id;
    std::string kind;
    std::string target;
    std::string decision;
    std::uint64_t mutation = 0;
};

struct LogState {
    std::mutex mu;
    std::string path;
    std::string run;
    std::uint64_t seq = 0;
    std::string tip;
    std::string last_error;
    // Issue #5251: bounded in-process trace. ring[i] and meta[i] are the same
    // record; both deques are trimmed together, so the ring never grows past
    // AURA_EVO_RING lines.
    std::deque<std::string> ring;
    std::deque<TraceMeta> meta;
    std::size_t ring_bytes = 0;
    std::uint64_t ring_dropped = 0;
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
    if (const char* r = ::aura::config::raw(::aura::config::Key::AURA_EVO_RUN); r && *r)
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
        const char* p = ::aura::config::raw(::aura::config::Key::AURA_EVO_LOG);
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

// Issue #5251: in-process trace capacity. AURA_EVO_RING=0 turns the ring
// off; AURA_EVO_LOG can still spill to a file. The file is no longer the
// switch that decides whether a record exists at all.
inline std::size_t ring_capacity() noexcept {
    return ::aura::config::size(::aura::config::Key::AURA_EVO_RING);
}

// True when a record can be kept anywhere (ring or file).
inline bool trace_enabled() noexcept {
    return ring_capacity() > 0 || log_enabled();
}

inline void json_escape_into(std::string& out, std::string_view in) {
    out.push_back('"');
    aura::json::escape_into(out, in);
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
    // Issue #5255 slot: the unified acceptance gate this row was decided by.
    // Empty today (nothing computes it yet); written only when set.
    std::string_view gate;
    // Issue #5255 slot: the gate's potential value as a raw JSON number.
    std::string_view weight_json;
    // Issue #5251: the workspace mutation-log id this row recorded (0 = none,
    // e.g. an evo:record! candidate row or a multi-row mutate).
    std::uint64_t mutation_id = 0;
    std::string_view extras_json; // #4939: pre-rendered "k":v,… members, or empty
    int version = 1;              // #4939: 1, or 2 when extras_json is set
};

// Appends one line; returns the new record id ("" when off or on I/O error).
// Cold audit write. A non-empty id is always run + '#' + sequence.
// GCC 16 evaluates the trailing post. The Clang tidy shim
// (cmake/clang_contract_shim.h) defines post(...) to nothing, so this
// declarator stays a plain function there. That shim is not the GCC check.
inline std::string log_append(const Entry& e)
    post(r : r.empty() || r.find('#') != std::string::npos) {
    // Issue #5251: log_enabled() fills path/run under the state lock, so it
    // runs before this scope takes the same lock.
    const bool file_on = log_enabled();
    const std::size_t ring_cap = ring_capacity();
    if (!file_on && ring_cap == 0)
        return {};
    auto& s = log_state();
    std::scoped_lock lk(s.mu);
    // A ring-only process still needs a run id: record ids are the join key
    // for "link" and the parent of the next candidate record.
    if (s.run.empty())
        s.run = default_run_id();
    const std::uint64_t seq = ++s.seq;
    std::string id = s.run + "#" + std::to_string(seq);
    const std::string parent = e.parent.empty() ? s.tip : std::string(e.parent);
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    std::string line;
    line.reserve(256 + e.old_src.size() + e.new_src.size() + e.evidence_json.size());
    line += "{\"v\":";
    line += std::to_string(e.version);
    line += ",\"id\":";
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
    // Issue #5255 slots: empty today, and written only when a gate fills them.
    if (!e.gate.empty()) {
        line += ",\"gate\":";
        json_escape_into(line, e.gate);
    }
    if (!e.weight_json.empty()) {
        line += ",\"weight\":";
        line += e.weight_json;
    }
    if (!e.extras_json.empty()) {
        line += ",";
        line += e.extras_json;
    }
    line += "}\n";
    // Issue #5251: keep the record in process whether or not a file is
    // configured, then trim oldest-first to the configured bound.
    if (ring_cap > 0 && line.size() <= kTraceRingBytes) {
        s.ring.push_back(line);
        s.ring_bytes += line.size();
        s.meta.push_back({id, std::string(e.kind), std::string(e.target), std::string(e.decision),
                          e.mutation_id});
        while (s.ring.size() > ring_cap || (s.ring_bytes > kTraceRingBytes && s.ring.size() > 1)) {
            s.ring_bytes -= s.ring.front().size();
            s.ring.pop_front();
            s.meta.pop_front();
            ++s.ring_dropped;
        }
    }
    if (!file_on)
        return id;
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
        // Issue #5251: the in-process record stands; the spill is best effort.
        return id;
    }
    // Issue #5139: only an audited candidate record is a worldline tip. An
    // auto-logged mutate:rebind / mw:restore reuses the candidate's decision
    // string ("KEEP"), but it is not a record: letting it advance the tip
    // made the next record name a rebind row as its parent, which the lineage
    // resolver (record ids or candidate names) rejects.
    if (e.decision == "KEEP" && e.kind == "record")
        s.tip = id;
    return id;
}

// Issue #5251: read the in-process ring, oldest first. Returns copies so a
// caller can json-parse outside the state lock.
struct TraceView {
    std::vector<std::string> lines;
    std::uint64_t dropped = 0;
    std::string run;
};

inline TraceView trace_snapshot() {
    auto& s = log_state();
    std::scoped_lock lk(s.mu);
    TraceView v;
    v.lines.assign(s.ring.begin(), s.ring.end());
    v.dropped = s.ring_dropped;
    v.run = s.run;
    return v;
}

// Issue #5251: the trace row that recorded workspace mutation `mutation_id`,
// if it is still in the ring. A restore ROLLBACK row uses it to link back to
// the row it undoes and to inherit its kind / target.
inline std::optional<TraceMeta> trace_origin(std::uint64_t mutation_id) {
    if (mutation_id == 0)
        return std::nullopt;
    auto& s = log_state();
    std::scoped_lock lk(s.mu);
    for (auto it = s.meta.rbegin(); it != s.meta.rend(); ++it) {
        if (it->mutation == mutation_id)
            return *it;
    }
    return std::nullopt;
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
    s.ring.clear();
    s.meta.clear();
    s.ring_bytes = 0;
    s.ring_dropped = 0;
    log_flag().store(-1, std::memory_order_release);
}

} // namespace aura::evo
