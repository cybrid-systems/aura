// DEPRECATED location for new work (#1959): prefer tests/domain/arena/
// (batch drivers + README). This file remains for bundle/history coverage.
//
// @category: integration
// @reason: uses CompilerService + (stats:get "compiler:metrics") primitive
//          to verify the 4 env_frames_/arena observability
//          counters are queryable post-implementation.
//
// test_issue_1385_env_arena_metrics.cpp — Issue #1385:
// env_frames_arena_bytes + stale_frame_count observability.
//
// Background: env_frames_ is append-only (monotonic growth in
// long-running processes). ASTArena's monotonic_buffer_resource
// doesn't reclaim upstream fallback chunks. Currently no
// observability surfaces this growth — operators can't tell when
// reclamation is overdue.
//
// This test verifies the 4 metrics are queryable via
// (stats:get "compiler:metrics") primitive (returns JSON string with the 4
// keys) and reflect state changes:
//   1. env_frames_size_total — current env_frames_.size()
//   2. env_frames_stale_count — frames with version_ < current
//      defuse_version_
//   3. ast_arena_bytes_in_use — ASTArena::used()
//   4. ast_arena_upstream_bytes — bytes via the arena's
//      CountingMR upstream
//
// Tests:
//   AC1: JSON returned by (stats:get "compiler:metrics") has all 4 keys
//        with non-negative integer values.
//   AC2: env_frames_size_total >= 1 after (set-code + eval).
//   AC3: env_frames_size_total grows monotonically across
//        multiple evals (append-only invariant).
//   AC4: ast_arena_bytes_in_use > 0 after workspace setup.

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core;
import aura.core.type;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.compiler.service;

namespace aura_issue_1385_detail {

// Helper: run an Aura expression via the service. Returns true
// on success.
static bool run_eval(aura::compiler::CompilerService& cs, const std::string& src) {
    std::string sexpr = std::format(R"X((eval "{}"))X", src);
    auto r = cs.eval(sexpr);
    if (!r) {
        std::println(std::cerr, "    [eval({}) failed: {}]", src, r.error().message);
        return false;
    }
    return true;
}

// Helper: set up a workspace via (set-code + eval-current).
static void setup_workspace(aura::compiler::CompilerService& cs, const std::string& src) {
    std::string sexpr = std::format(R"X((set-code "{}"))X", src);
    auto r = cs.eval(sexpr);
    if (!r) {
        std::println(std::cerr, "    [eval(set-code) failed: {}]", r.error().message);
    }
    run_eval(cs, "(eval-current)");
}

// Helper: query (stats:get "compiler:metrics") and return the JSON string.
// Returns empty string on failure.
static std::string query_compiler_metrics(aura::compiler::CompilerService& cs) {
    auto r = cs.eval("(stats:get \"compiler:metrics\")");
    if (!r)
        return std::string{};
    if (!aura::compiler::types::is_string(*r))
        return std::string{};
    auto idx = aura::compiler::types::as_string_idx(*r);
    auto heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return std::string{};
    return heap[idx];
}

// Helper: parse a uint64 out of the JSON for a specific key.
// Very simple parser — looks for `"key":<number>` pattern.
static std::uint64_t parse_uint64(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos)
        return UINT64_MAX; // sentinel: key not found
    pos += needle.size();
    std::uint64_t v = 0;
    bool any = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        v = v * 10 + static_cast<std::uint64_t>(json[pos] - '0');
        ++pos;
        any = true;
    }
    return any ? v : UINT64_MAX;
}

// ── AC1: JSON has all 4 keys with non-negative integer values ──
bool test_ac1_json_has_all_four_keys() {
    std::println("\n--- AC1: (stats:get \"compiler:metrics\") JSON has all 4 keys ---");
    aura::compiler::CompilerService cs;

    std::string json = query_compiler_metrics(cs);
    CHECK(!json.empty(), "AC1: (stats:get \"compiler:metrics\") returned a non-empty string");

    auto sz = parse_uint64(json, "env_frames_size_total");
    auto stale = parse_uint64(json, "env_frames_stale_count");
    auto in_use = parse_uint64(json, "ast_arena_bytes_in_use");
    auto upstream = parse_uint64(json, "ast_arena_upstream_bytes");

    CHECK(sz != UINT64_MAX, "AC1: env_frames_size_total key present");
    CHECK(stale != UINT64_MAX, "AC1: env_frames_stale_count key present");
    CHECK(in_use != UINT64_MAX, "AC1: ast_arena_bytes_in_use key present");
    CHECK(upstream != UINT64_MAX, "AC1: ast_arena_upstream_bytes key present");
    std::println("  AC1: sizes={} stale={} in_use={} upstream={}", sz, stale, in_use, upstream);
    return true;
}

// ── AC2: env_frames_size_total >= 1 after eval ─────────────────
bool test_ac2_size_total_nonzero_after_eval() {
    std::println("\n--- AC2: env_frames_size_total >= 1 after eval ---");
    aura::compiler::CompilerService cs;

    // Initial snapshot.
    std::string j0 = query_compiler_metrics(cs);
    auto sz0 = parse_uint64(j0, "env_frames_size_total");

    setup_workspace(cs, "(define x 1) (define y 2)");

    std::string j1 = query_compiler_metrics(cs);
    auto sz1 = parse_uint64(j1, "env_frames_size_total");

    std::println("  AC2: size before={} after={}", sz0, sz1);
    CHECK(sz1 >= sz0, "AC2: size_total non-decreasing (append-only)");
    CHECK(sz1 >= 1, "AC2: size_total >= 1 after eval (frame allocated)");
    return true;
}

// ── AC3: size grows monotonically across multiple evals ────────
bool test_ac3_size_grows_monotonically() {
    std::println("\n--- AC3: env_frames_size_total grows monotonically ---");
    aura::compiler::CompilerService cs;
    setup_workspace(cs, "(define a 1) (define b 2) (define c 3)");

    auto sz_a = parse_uint64(query_compiler_metrics(cs), "env_frames_size_total");
    run_eval(cs, "(define d 4)");
    auto sz_b = parse_uint64(query_compiler_metrics(cs), "env_frames_size_total");
    run_eval(cs, "(define e 5)");
    auto sz_c = parse_uint64(query_compiler_metrics(cs), "env_frames_size_total");

    std::println("  AC3: sizes over evals: {} → {} → {}", sz_a, sz_b, sz_c);
    CHECK(sz_b >= sz_a, "AC3: size_total non-decreasing across eval 1");
    CHECK(sz_c >= sz_b, "AC3: size_total non-decreasing across eval 2");
    return true;
}

// ── AC4: ast_arena_bytes_in_use > 0 after workspace setup ──────
bool test_ac4_arena_bytes_in_use_nonzero() {
    std::println("\n--- AC4: ast_arena_bytes_in_use > 0 after setup ---");
    aura::compiler::CompilerService cs;
    setup_workspace(cs, "(define a 1) (define b 2) (define c 3)");

    auto in_use = parse_uint64(query_compiler_metrics(cs), "ast_arena_bytes_in_use");
    std::println("  AC4: ast_arena_bytes_in_use={}", in_use);
    CHECK(in_use > 0, "AC4: ast_arena_bytes_in_use > 0 (arena holds the parsed AST)");
    return true;
}

} // namespace aura_issue_1385_detail

int main() {
    using namespace aura_issue_1385_detail;
    bool ok = true;
    ok &= test_ac1_json_has_all_four_keys();
    ok &= test_ac2_size_total_nonzero_after_eval();
    ok &= test_ac3_size_grows_monotonically();
    ok &= test_ac4_arena_bytes_in_use_nonzero();

    int failed = aura::test::g_failed;
    std::println("\n=== Issue #1385 env+arena metrics: {} ({} failures) ===",
                 (ok && failed == 0) ? "PASS" : "FAIL", failed);
    return (ok && failed == 0) ? 0 : 1;
}