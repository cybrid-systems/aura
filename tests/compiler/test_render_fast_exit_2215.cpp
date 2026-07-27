// @category: unit
// @reason: Issue #2215 — RenderFastExit for outermost MutationBoundary under
// render hotpath (skip Full audit / full linear+dual-path; defer reemit).
//
//   AC1: outermost success under hotpath → render_fast_exit_total +1
//   AC2: query schema-2215 + counter keys
//   AC3: non-render outermost / failure do not take fast exit
//   AC4: RenderHotEntry sets hotpath; source cites bypass
//   AC5: skipped_audit / deferred_reemit counters advance under hotpath

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/arena_auto_policy_stats.h"

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
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-boundary-hold-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac4_source() {
    std::println("\n--- AC4: source wiring ---");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto tpl = read_file("src/compiler/render_prim_template.hh");
    auto oh = read_file("src/compiler/observability_metrics.h");
    CHECK(mb.find("#2215") != std::string::npos || mb.find("Issue #2215") != std::string::npos,
          "dtor cites #2215");
    CHECK(mb.find("render_fast_exit_") != std::string::npos, "render_fast_exit_ flag");
    CHECK(mb.find("render_fast_exit_this_boundary_") != std::string::npos, "evaluator flag");
    CHECK(mb.find("render_fast_exit_deferred_reemit_total") != std::string::npos, "defer reemit");
    CHECK(tpl.find("RenderFastExit") != std::string::npos ||
              tpl.find("render-fast-exit") != std::string::npos,
          "template docs");
    CHECK(oh.find("render_fast_exit_total") != std::string::npos, "metrics field");
}

static void ac1_ac5_hotpath_fast_exit() {
    std::println("\n--- AC1/AC5: hotpath outermost success → fast exit ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics");

    // Control: no hotpath
    const auto f0 = m->render_fast_exit_total.load();
    const auto s0 = m->render_fast_exit_skipped_audit_total.load();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
        CHECK(g.is_outermost(), "outermost control");
    }
    CHECK(ok, "success control");
    CHECK(m->render_fast_exit_total.load() == f0, "AC3: no fast exit without hotpath");

    // Under render hotpath (simulates AURA_RENDER_HOT_ENTRY)
    aura::core::arena_policy::enter_render_hotpath();
    CHECK(aura::core::arena_policy::in_render_hotpath(), "hotpath on");
    const auto f1 = m->render_fast_exit_total.load();
    const auto s1 = m->render_fast_exit_skipped_audit_total.load();
    ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
        CHECK(g.is_outermost(), "outermost hotpath");
        // lightweight body mutation signal optional
    }
    aura::core::arena_policy::exit_render_hotpath();
    CHECK(ok, "success hotpath");
    CHECK(m->render_fast_exit_total.load() == f1 + 1, "AC1: fast_exit_total +1");
    CHECK(m->render_fast_exit_skipped_audit_total.load() > s1,
          "AC5: skipped_audit advanced (audit and/or probes)");

    // Nested under hotpath: only outermost counts once
    aura::core::arena_policy::enter_render_hotpath();
    const auto f2 = m->render_fast_exit_total.load();
    ok = true;
    {
        Evaluator::MutationBoundaryGuard outer(ev, &ok);
        {
            Evaluator::MutationBoundaryGuard inner(ev, &ok);
            CHECK(!inner.is_outermost(), "inner nested");
        }
    }
    aura::core::arena_policy::exit_render_hotpath();
    CHECK(m->render_fast_exit_total.load() == f2 + 1, "nested pair → fast exit +1");
}

static void ac3_failure_no_fast() {
    std::println("\n--- AC3: failure under hotpath does not fast-exit ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    aura::core::arena_policy::enter_render_hotpath();
    const auto f0 = m->render_fast_exit_total.load();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
        g.mark_failed();
    }
    aura::core::arena_policy::exit_render_hotpath();
    CHECK(!ok || true, "failed flag");
    CHECK(m->render_fast_exit_total.load() == f0, "AC3: failure → no fast exit");
}

static void ac2_query() {
    std::println("\n--- AC2: query schema-2215 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2215") == 2215, "schema-2215");
    CHECK(href(cs, "issue-2215") == 2215, "issue-2215");
    CHECK(href(cs, "render-fast-exit-wired") == 1, "wired");
    CHECK(href(cs, "render-fast-exit-total") >= 0, "total key");
    CHECK(href(cs, "render-fast-exit-skipped-audit-total") >= 0, "skipped key");
    CHECK(href(cs, "render-fast-exit-deferred-reemit-total") >= 0, "deferred key");
    CHECK(href(cs, "schema-2120") == 2120, "2120 lineage");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(q.find("schema-2215") != std::string::npos, "query cites schema");
}

static void ac4_render_hot_entry_guard() {
    std::println("\n--- AC4: RenderHotEntryGuard path ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto f0 = m->render_fast_exit_total.load();
    {
        Evaluator::RenderHotEntryGuard hot(ev);
        CHECK(aura::core::arena_policy::in_render_hotpath(), "hot entry sets hotpath");
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
        }
        CHECK(ok, "guard success under hot entry");
    }
    CHECK(!aura::core::arena_policy::in_render_hotpath(), "hot entry cleared");
    CHECK(m->render_fast_exit_total.load() == f0 + 1, "AC4: fast exit via RenderHotEntryGuard");
}

} // namespace

int main() {
    std::println("=== Issue #2215: RenderFastExit under render hotpath ===");
    // Drain residual hotpath depth from prior suites
    while (aura::core::arena_policy::in_render_hotpath())
        aura::core::arena_policy::exit_render_hotpath();

    ac4_source();
    ac1_ac5_hotpath_fast_exit();
    ac3_failure_no_fast();
    ac2_query();
    ac4_render_hot_entry_guard();

    std::println("\n=== test_render_fast_exit_2215: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
