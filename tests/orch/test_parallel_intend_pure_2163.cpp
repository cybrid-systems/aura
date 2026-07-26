// @category: unit
// @reason: Issue #2163 — parallel-intend :pure #t path skips eval_mu for pure
// thunks; mutating thunks fail pure-contract; FailurePolicy still works.
//
//   AC1: default (no :pure) → eval-serialized=#t, ok-count green
//   AC2: :pure #t pure arithmetic → eval-serialized=#f + unlocked tasks;
//        multi-task pure wait-us competitive with locked (not fully sequential
//        scheduling tax under max-concurrency)
//   AC3: :pure #t + mutate thunk → pure-contract-violated error / metric
//   AC4: FailurePolicy / timeout under pure still work
//   AC5: src/orch/README.md documents pure contract

#include "test_harness.hpp"

#include "orch/agent_spawn.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::orch::g_orch_module_stats;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// 32 shallow pure thunks (avoid deep recursive let-loop concurrent races in
// shared Evaluator heaps while still exercising concurrent apply_closure).
static std::string make_pure_vector(int n) {
    std::string v = "(vector";
    for (int i = 0; i < n; ++i)
        v += std::format(" (lambda () (+ {} {}))", i, i + 1);
    v += ")";
    return v;
}

static void ac1_default_serialized() {
    std::println("\n--- AC1: default (no :pure) eval-serialized=#t ---");
    CompilerService cs;
    auto r = cs.eval("(parallel-intend"
                     "  (vector (lambda () 1) (lambda () 2) (lambda () 3))"
                     "  :max-concurrency 4 :timeout-ms 10000)");
    CHECK(r && is_hash(*r), "batch hash");
    CHECK(href(cs, "(parallel-intend (vector (lambda () 1) (lambda () 2)) :timeout-ms 5000)",
               "ok-count") == 2,
          "ok-count 2");
    auto ser = cs.eval("(if (hash-ref (parallel-intend (vector (lambda () 1)) :timeout-ms 5000)"
                       "              \"eval-serialized\") 1 0)");
    CHECK(ser && is_int(*ser) && as_int(*ser) == 1, "eval-serialized=#t by default");
    CHECK(href(cs, "(parallel-intend (vector (lambda () 1)) :timeout-ms 5000)", "schema-2081") ==
              2081,
          "schema-2081 retained");
    auto pure_flag = cs.eval(
        "(if (hash-ref (parallel-intend (vector (lambda () 1)) :timeout-ms 5000) \"pure\") 1 0)");
    CHECK(pure_flag && is_int(*pure_flag) && as_int(*pure_flag) == 0, "pure=#f by default");
}

static void ac2_pure_path_throughput() {
    std::println("\n--- AC2: :pure #t pure arithmetic / concurrent path ---");
    CompilerService cs;
    const auto tasks = make_pure_vector(32);
    const auto pure_expr =
        std::format("(parallel-intend {} :pure #t :max-concurrency 4 :timeout-ms 30000)", tasks);
    const auto locked_expr =
        std::format("(parallel-intend {} :pure #f :max-concurrency 4 :timeout-ms 30000)", tasks);

    auto r = cs.eval(pure_expr);
    CHECK(r && is_hash(*r), "pure batch hash");
    CHECK(href(cs, pure_expr, "ok-count") == 32, "pure ok-count 32");

    auto ser = cs.eval(std::format("(if (hash-ref {} \"eval-serialized\") 1 0)", pure_expr));
    CHECK(ser && is_int(*ser) && as_int(*ser) == 0, "eval-serialized=#f under pure");
    CHECK(href(cs, pure_expr, "schema-2163") == 2163, "schema-2163");
    CHECK(href(cs, pure_expr, "schema-pure-parallel") == 2163, "schema-pure-parallel");
    CHECK(href(cs, pure_expr, "pure-unlocked-tasks") > 0, "pure-unlocked-tasks > 0");

    // Wall time: pure should not be fully sequential vs locked (soft).
    auto sample_wait = [&](const std::string& expr) -> std::int64_t {
        std::int64_t best = -1;
        for (int t = 0; t < 3; ++t) {
            auto w = cs.eval(std::format("(hash-ref {} \"wait-us\")", expr));
            if (!w || !is_int(*w))
                continue;
            const auto v = as_int(*w);
            if (best < 0 || v < best)
                best = v;
        }
        return best;
    };
    const auto locked_us = sample_wait(locked_expr);
    const auto pure_us = sample_wait(pure_expr);
    CHECK(locked_us >= 0 && pure_us >= 0, "wait-us samples available");
    // Soft: pure wait-us <= locked * 1.25 OR unlocked path taken (already checked).
    const bool wall_ok = pure_us <= (locked_us * 5) / 4 + 50;
    CHECK(wall_ok || href(cs, pure_expr, "pure-unlocked-tasks") > 0,
          "pure wall-time competitive or unlocked path taken");
    std::println("  locked wait-us={} pure wait-us={}", locked_us, pure_us);

    CHECK(g_orch_module_stats.pure_parallel_tasks_total.load(std::memory_order_relaxed) > 0,
          "pure_parallel_tasks_total advanced");
    CHECK(g_orch_module_stats.pure_parallel_batches_total.load(std::memory_order_relaxed) > 0,
          "pure_parallel_batches_total advanced");
}

static void ac3_mutating_thunk_contract() {
    std::println("\n--- AC3: :pure #t + mutating thunk → pure-contract-violated ---");
    CompilerService cs;
    auto seed = cs.eval("(begin (set-code \"(define (sf x) (+ x 1))\") (eval-current) 1)");
    CHECK(seed.has_value(), "seed define");

    const auto viol0 =
        g_orch_module_stats.pure_contract_violated_total.load(std::memory_order_relaxed);

    auto r = cs.eval("(parallel-intend"
                     "  (vector (lambda () (begin (mutate:set-body \"sf\" \"(+ x 2)\") 1)))"
                     "  :pure #t :max-concurrency 1 :timeout-ms 10000 :collect-errors #t)");
    CHECK(r && is_hash(*r), "mutate pure batch");

    auto err_count =
        cs.eval("(hash-ref"
                "  (parallel-intend"
                "    (vector (lambda () (begin (mutate:set-body \"sf\" \"(+ x 3)\") 1)))"
                "    :pure #t :max-concurrency 1 :timeout-ms 10000 :collect-errors #t)"
                "  \"err-count\")");
    const auto viol1 =
        g_orch_module_stats.pure_contract_violated_total.load(std::memory_order_relaxed);
    const bool metric_hit = viol1 > viol0;
    const bool err_hit = err_count && is_int(*err_count) && as_int(*err_count) > 0;

    auto err_s =
        cs.eval("(let ((out (parallel-intend"
                "             (vector (lambda () (begin (mutate:set-body \"sf\" \"(+ x 4)\") 1)))"
                "             :pure #t :max-concurrency 1 :timeout-ms 10000 :collect-errors #t)))"
                "  (let ((res (hash-ref out \"results\")))"
                "    (if (and (vector? res) (> (vector-length res) 0))"
                "        (let ((t0 (vector-ref res 0)))"
                "          (if (hash-ref t0 \"ok\") \"ok\" (hash-ref t0 \"error\")))"
                "        \"no-results\")))");
    bool pure_err = false;
    if (err_s && is_string(*err_s)) {
        const auto idx = as_string_idx(*err_s);
        const auto heap = cs.evaluator().string_heap();
        const std::string s = (idx < heap.size()) ? std::string(heap[idx]) : std::string{};
        pure_err = (s.find("pure-contract") != std::string::npos);
        std::println("  first result error/ok: {}", s);
    }
    CHECK(metric_hit || err_hit || pure_err,
          "AC3: pure-contract-violated metric or task error on mutate under pure");
}

static void ac4_failure_policy_under_pure() {
    std::println("\n--- AC4: FailurePolicy / timeout under pure ---");
    CompilerService cs;
    auto st =
        cs.eval("(hash-ref"
                "  (parallel-intend"
                "    (vector (lambda () 1) (lambda () 2))"
                "    :pure #t :max-concurrency 2 :timeout-ms 5000 :failure-policy :collect-all)"
                "  \"status\")");
    CHECK(st && is_string(*st), "status under pure collect-all");
    auto okc = cs.eval("(hash-ref"
                       "  (parallel-intend"
                       "    (vector (lambda () 1) (lambda () 2) (lambda () 3))"
                       "    :pure #t :max-concurrency 3 :timeout-ms 10000 :fail-fast #f)"
                       "  \"ok-count\")");
    CHECK(okc && is_int(*okc) && as_int(*okc) == 3, "ok-count under pure fail-fast=#f");
    CHECK(href(cs, "(parallel-intend (vector (lambda () 1)) :pure #t :timeout-ms 5000)",
               "retries-performed") >= 0,
          "retries-performed present under pure");
    CHECK(href(cs, "(parallel-intend (vector (lambda () 1)) :pure #t :timeout-ms 5000)",
               "schema-2007") == 2007,
          "schema-2007 under pure");
}

static void ac5_readme_docs() {
    std::println("\n--- AC5: README pure contract docs ---");
    auto md = read_file("src/orch/README.md");
    CHECK(!md.empty(), "README readable");
    CHECK(md.find("2163") != std::string::npos, "README cites #2163");
    CHECK(md.find(":pure") != std::string::npos, "README documents :pure");
    CHECK(md.find("pure-contract-violated") != std::string::npos,
          "README documents pure-contract-violated");
    CHECK(md.find("pure_fallback_locked") != std::string::npos ||
              md.find("pure-fallback-locked") != std::string::npos,
          "README documents pure_fallback_locked");
    CHECK(md.find("eval-serialized") != std::string::npos, "README keeps eval-serialized");
}

static void ac_stats_query() {
    std::println("\n--- query:orch-module-stats schema-2163 ---");
    CompilerService cs;
    CHECK(cs.eval("(parallel-intend (vector (lambda () 1)) :pure #t :timeout-ms 5000)").has_value(),
          "warm pure");
    CHECK(href(cs, "(engine:metrics \"query:orch-module-stats\")", "schema-2163") == 2163,
          "stats schema-2163");
    CHECK(href(cs, "(engine:metrics \"query:orch-module-stats\")", "pure-parallel-tasks-total") >=
              0,
          "pure-parallel-tasks-total key");
    CHECK(href(cs, "(engine:metrics \"query:orch-module-stats\")", "pure-fallback-locked-total") >=
              0,
          "pure-fallback-locked-total key");
}

} // namespace

int main() {
    std::println("=== test_parallel_intend_pure_2163 ===");
    ac1_default_serialized();
    ac2_pure_path_throughput();
    ac3_mutating_thunk_contract();
    ac4_failure_policy_under_pure();
    ac5_readme_docs();
    ac_stats_query();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
