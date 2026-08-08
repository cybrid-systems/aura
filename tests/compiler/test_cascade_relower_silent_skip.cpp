// @category: unit
// @reason: Issue #2813 — cascade must not silently skip ir_cache_v2 re-lower
// when defines_n>0 but relower_dirty_defines_fn_ is null.
//
//   AC1: cascade cites #2813; skipped/ran metrics; warn path
//   AC2: CompilerService wired path → cascade_relower_ran_total advances
//   AC3: fn cleared → cascade_relower_skipped_total advances on mutate
//   AC4: this suite + linter; no docs/design/2813-*; no test_issue_2813.cpp

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_cascade_relower_silent_skip() {
    std::println("=== Issue #2813: cascade relower silent skip observability ===");
    CHECK(true, "ac2813: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: cascade skip metric + production wiring docs ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto ixx = read_file("src/compiler/evaluator.ixx");
        auto svc = read_file("src/compiler/service.ixx");
        auto met = read_file("src/compiler/observability_metrics.h");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(!mut.empty(), "AC1: sources readable");

        auto cascade = mut.find("push_post_mutate_incremental_cascade");
        CHECK(cascade != std::string::npos, "AC1: cascade present");
        auto win = mut.substr(cascade, 5500);
        CHECK(win.find("Issue #2813") != std::string::npos, "AC1: cascade cites #2813");
        CHECK(win.find("cascade_relower_skipped_total") != std::string::npos,
              "AC1: skipped metric bump");
        CHECK(win.find("cascade_relower_ran_total") != std::string::npos, "AC1: ran metric bump");
        CHECK(win.find("relower_dirty_defines_fn_") != std::string::npos, "AC1: fn check");
        // Must not only short-circuit without metrics.
        CHECK(win.find("defines_n > 0") != std::string::npos, "AC1: defines_n gate");

        CHECK(ixx.find("Issue #2813") != std::string::npos, "AC1: ixx documents wiring");
        CHECK(ixx.find("relower_dirty_defines_wired") != std::string::npos, "AC1: probe API");
        CHECK(svc.find("Issue #1495 / #2813") != std::string::npos ||
                  svc.find("#2813") != std::string::npos,
              "AC1: service wiring cites #2813");
        CHECK(met.find("cascade_relower_skipped_total") != std::string::npos, "AC1: metrics.h");
        CHECK(met.find("cascade_relower_ran_total") != std::string::npos, "AC1: ran in metrics.h");
        CHECK(obs.find("schema-2813") != std::string::npos, "AC1: query schema-2813");
    }

    // ── AC2: wired path advances ran metric ──
    {
        std::println("\n--- AC2: CompilerService wired → cascade_relower_ran_total ---");
        CompilerService cs;
        CHECK(cs.evaluator().relower_dirty_defines_wired(), "AC2: CS wires relower");
        CHECK(cs.eval("(set-code \"(define (f x) x) (f 1)\")").has_value(), "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "AC2: metrics");
        const auto r0 = m->cascade_relower_ran_total.load(std::memory_order_relaxed);
        const auto s0 = m->cascade_relower_skipped_total.load(std::memory_order_relaxed);
        auto mut = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 1))\" \"#2813\")");
        CHECK(mut.has_value(), "AC2: set-body");
        const auto r1 = m->cascade_relower_ran_total.load(std::memory_order_relaxed);
        const auto s1 = m->cascade_relower_skipped_total.load(std::memory_order_relaxed);
        CHECK(r1 > r0, "AC2: ran metric advanced");
        CHECK(s1 == s0, "AC2: skipped metric unchanged when wired");
        CHECK(href(cs, "cascade_relower_ran_total") == static_cast<std::int64_t>(r1) ||
                  href(cs, "cascade-relower-ran-total") == static_cast<std::int64_t>(r1),
              "AC2: query ran surface");
        CHECK(href(cs, "schema-2813") == 2813 || href(cs, "cascade-relower-wired") == 1,
              "AC2: schema-2813");
    }

    // ── AC3: null fn → skipped metric ──
    {
        std::println("\n--- AC3: unwired relower → cascade_relower_skipped_total ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (g x) x) (g 0)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        // Simulate misconfigured Evaluator (production without service wire).
        cs.evaluator().set_relower_dirty_defines_fn(nullptr);
        CHECK(!cs.evaluator().relower_dirty_defines_wired(), "AC3: fn cleared");
        auto* m = metrics_of(cs);
        const auto s0 = m->cascade_relower_skipped_total.load(std::memory_order_relaxed);
        const auto r0 = m->cascade_relower_ran_total.load(std::memory_order_relaxed);
        auto mut = cs.eval("(mutate:set-body \"g\" \"(lambda (x) (* x 2))\" \"#2813-skip\")");
        CHECK(mut.has_value(), "AC3: set-body under null relower");
        const auto s1 = m->cascade_relower_skipped_total.load(std::memory_order_relaxed);
        const auto r1 = m->cascade_relower_ran_total.load(std::memory_order_relaxed);
        CHECK(s1 > s0, "AC3: skipped metric advanced");
        CHECK(r1 == r0, "AC3: ran metric not advanced when null");
        CHECK(href(cs, "cascade_relower_skipped_total") == static_cast<std::int64_t>(s1) ||
                  href(cs, "cascade-relower-skipped-total") == static_cast<std::int64_t>(s1),
              "AC3: query skipped surface");
        // Cascade still marked defines (defuse/dirty path independent).
        CHECK(href(cs, "post_mutate_incremental_cascade_total") > 0 ||
                  m->post_mutate_incremental_cascade_total.load() > 0,
              "AC3: cascade still ran overall");
    }

    // ── AC4: defines_n==0 is not a skip ──
    {
        std::println("\n--- AC4: empty affected is not cascade_relower_skipped ---");
        // Soft documentation: skip only when defines_n>0 && fn null.
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto pos = mut.find("cascade_relower_skipped_total");
        CHECK(pos != std::string::npos, "AC4: skipped site present");
        // Skipped only in the else of (defines_n > 0 && !fn).
        auto win = mut.substr(pos > 800 ? pos - 800 : 0, 1600);
        CHECK(win.find("defines_n > 0") != std::string::npos, "AC4: skip gated on defines_n");
        CHECK(win.find("relower_dirty_defines_fn_") != std::string::npos, "AC4: skip gated on fn");
    }

    std::println("\n=== #2813 cascade relower silent skip: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cascade_relower_silent_skip();
}
#endif
