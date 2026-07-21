// @category: integration
// @reason: Issue #1909 — unified query:self-evo-stats self-evolution loop
// health dashboard (macro ratio, hygiene rate, IR propagation, depth,
// rollback success, latency p99, health-score).
//
//   AC1: source registers query:self-evo-stats schema 1909
//   AC2: engine:metrics returns hash with AC keys
//   AC3: ratios in 0..10000 bp; recommendation in 0..2
//   AC4: after set-code/eval, counters and ratios still well-formed
//   AC5: multi-query monotonic self_evo_unified_health_queries_total
//   AC6: lineage flags for 1891/1892/1894/1883 present

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:self-evo-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static bool in_bp(std::int64_t v) {
    return v >= 0 && v <= 10000;
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: query:self-evo-stats registration ---");
        std::string src, cat;
        for (const char* p : {"src/compiler/evaluator_primitives_query.cpp",
                              "../src/compiler/evaluator_primitives_query.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        for (const char* p : {"src/compiler/evaluator_primitives_observability.cpp",
                              "../src/compiler/evaluator_primitives_observability.cpp"}) {
            cat = read_file(p);
            if (!cat.empty())
                break;
        }
        CHECK(!src.empty(), "read query.cpp");
        CHECK(src.find("query:self-evo-stats") != std::string::npos, "registers name");
        CHECK(src.find("#1909") != std::string::npos, "cites #1909");
        CHECK(src.find("macro-introduced-ratio-bp") != std::string::npos, "ratio key");
        CHECK(src.find("hygiene-violation-rate-bp") != std::string::npos, "hygiene rate key");
        CHECK(src.find("ir-macro-propagated-pct-bp") != std::string::npos, "ir pct key");
        CHECK(src.find("self-evo-loop-latency-p99-us") != std::string::npos, "latency key");
        CHECK(src.find("health-score-bp") != std::string::npos, "health score");
        CHECK(!cat.empty() && cat.find("query:self-evo-stats") != std::string::npos,
              "catalog lists name");
    }

    // ── AC2: hash surface ──
    {
        std::println("\n--- AC2: stats hash AC keys ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:self-evo-stats\")");
        CHECK(r.has_value() && is_hash(*r), "is hash");
        CHECK(href(cs, "schema") == 1909, "schema 1909");
        CHECK(href(cs, "issue") == 1909, "issue 1909");
        CHECK(href(cs, "active") == 1, "active");
        CHECK(href(cs, "unified-dashboard-wired") == 1, "wired flag");
        CHECK(href(cs, "macro-introduced-ratio-bp") >= 0, "macro ratio");
        CHECK(href(cs, "hygiene-violation-rate-bp") >= 0, "hygiene rate");
        CHECK(href(cs, "ir-macro-propagated-pct-bp") >= 0, "ir pct");
        CHECK(href(cs, "avg-mutation-boundary-depth-x100") >= 0, "depth");
        CHECK(href(cs, "rollback-success-rate-bp") >= 0, "rollback rate");
        CHECK(href(cs, "self-evo-loop-latency-p99-us") >= 0, "latency p99");
        CHECK(href(cs, "health-score-bp") >= 0, "health score");
        CHECK(href(cs, "recommendation") >= 0 && href(cs, "recommendation") <= 2, "rec 0..2");
        // snake_case AC aliases
        CHECK(href(cs, "macro_introduced_ratio") >= 0, "snake macro ratio");
        CHECK(href(cs, "hygiene_violation_rate") >= 0, "snake hygiene");
        CHECK(href(cs, "ir_macro_propagated_pct") >= 0, "snake ir");
        CHECK(href(cs, "rollback_success_rate") >= 0, "snake rollback");
        CHECK(href(cs, "self_evo_loop_latency_p99") >= 0, "snake latency");
    }

    // ── AC3: well-formed rates ──
    {
        std::println("\n--- AC3: rates in basis-point range ---");
        CompilerService cs;
        CHECK(in_bp(href(cs, "macro-introduced-ratio-bp")), "macro ratio bp");
        CHECK(in_bp(href(cs, "hygiene-violation-rate-bp")), "hygiene rate bp");
        CHECK(in_bp(href(cs, "ir-macro-propagated-pct-bp")), "ir pct bp");
        CHECK(in_bp(href(cs, "rollback-success-rate-bp")), "rollback bp");
        CHECK(in_bp(href(cs, "health-score-bp")), "health bp");
        CHECK(in_bp(href(cs, "invariant-pass-rate-bp")), "invariant bp");
    }

    // ── AC4: after workload ──
    {
        std::println("\n--- AC4: after set-code / eval ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1))) (define g (* 2 3))\")")
                  .has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        CHECK(href(cs, "workspace-nodes") >= 0, "workspace nodes");
        CHECK(href(cs, "schema") == 1909, "schema holds");
        CHECK(in_bp(href(cs, "health-score-bp")), "health after work");
        // IR module may or may not exist yet — keys still present.
        CHECK(href(cs, "ir-module-walked") >= 0, "ir walked key");
        CHECK(href(cs, "ir-instr-total") >= 0, "ir total key");
    }

    // ── AC5: query counter monotonic ──
    {
        std::println("\n--- AC5: self_evo_unified_health_queries_total ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "metrics");
        const auto before = load_u64(m->self_evo_unified_health_queries_total);
        for (int i = 0; i < 5; ++i)
            (void)cs.eval("(engine:metrics \"query:self-evo-stats\")");
        const auto after = load_u64(m->self_evo_unified_health_queries_total);
        CHECK(after >= before + 5, "queries bumped ≥5");
    }

    // ── AC6: lineage flags ──
    {
        std::println("\n--- AC6: lineage wire flags ---");
        CompilerService cs;
        CHECK(href(cs, "ir-hygiene-lineage") == 1891, "ir lineage");
        CHECK(href(cs, "pattern-hygiene-lineage") == 1892, "pattern lineage");
        CHECK(href(cs, "typed-audit-lineage") == 1894, "audit lineage");
        CHECK(href(cs, "loop-stats-lineage") == 1883, "loop lineage");
        CHECK(href(cs, "schema") == 1909, "schema after lineage check");
    }

    std::println("\n=== test_self_evo_stats_1909: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
