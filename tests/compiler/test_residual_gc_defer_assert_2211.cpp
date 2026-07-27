// @category: unit
// @reason: Issue #2211 — residual GcDeferReason assert at outermost Guard exit.
//
//   AC1: Success path of outermost exit leaves defer_reasons_snapshot()==0
//        under normal nesting (and residual counter stays 0).
//   AC2: Intentional residual (extra MutationHold arm) bumps
//        mutation_boundary_residual_defer_total and is cleared by best-effort.
//   AC3: Existing #2120 / #2088 / #2086 contracts retained (source + schema).
//   AC4: query:mutation-boundary-hold-stats schema-2211 + residual keys.

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/gc_hooks.h"

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

static void drain_known_defer() {
    while (aura::gc_hooks::mutation_hold_defer_active())
        aura::gc_hooks::release_mutation_hold_defer();
    while (aura::gc_hooks::gc_defer_pending_panic_depth() > 0)
        aura::gc_hooks::release_gc_defer_pending_panic();
}

static void ac1_normal_success_no_residual() {
    std::println("\n--- AC1: outermost success leaves snapshot==0 ---");
    drain_known_defer();
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics wired");

    const auto r0 = m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed);
    const auto done0 = m->outermost_exit_order_complete_total.load(std::memory_order_relaxed);
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
        CHECK(ok, "guard acquired");
        CHECK(g.is_outermost(), "outermost");
        // Nested Guard should not leave residual either.
        {
            Evaluator::MutationBoundaryGuard inner(ev, &ok);
            CHECK(!inner.is_outermost(), "inner nested");
        }
    }
    CHECK(ok, "success flag");
    CHECK(aura::gc_hooks::defer_reasons_snapshot() == 0,
          "AC1: defer_reasons_snapshot()==0 after outermost success");
    CHECK(!aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(&ev)),
          "AC1: no residual panic-defer for this evaluator");
    CHECK(!aura::gc_hooks::mutation_hold_defer_active(), "AC1: MutationHold inactive");
    CHECK(m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed) == r0,
          "AC1: residual counter unchanged under normal nesting");
    CHECK(m->outermost_exit_order_complete_total.load(std::memory_order_relaxed) == done0 + 1,
          "AC1: order complete +1");
}

static void ac2_inject_residual_bumps_and_clears() {
    std::println("\n--- AC2: inject residual MutationHold → counter + clear ---");
    drain_known_defer();
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics");

    const auto r0 = m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed);
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
        CHECK(ok && g.is_outermost(), "AC2: outermost acquire");
        // Intentional residual: extra MutationHold arm (depth 2). Phase5
        // release drops to depth 1 → bit stays → residual check fires.
        aura::gc_hooks::arm_mutation_hold_defer();
        CHECK(aura::gc_hooks::mutation_hold_defer_depth() >= 2,
              "AC2: pre: extra arm raised hold depth");
    }
    CHECK(m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed) == r0 + 1,
          "AC2: residual counter +1 after inject");
    CHECK(!aura::gc_hooks::mutation_hold_defer_active(),
          "AC2: best-effort clear released residual MutationHold");
    CHECK((aura::gc_hooks::defer_reasons_snapshot() &
           static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::MutationHold)) == 0,
          "AC2: MutationHold bit clear after best-effort");
    // Process-wide Panic may linger only if other tests left it; drain for hygiene.
    drain_known_defer();
    CHECK(aura::gc_hooks::defer_reasons_snapshot() == 0 ||
              !aura::gc_hooks::mutation_hold_defer_active(),
          "AC2: hold residual gone");
}

static void ac3_lineage_retained() {
    std::println("\n--- AC3: #2120 / #2088 / #2086 lineage retained ---");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto hooks = read_file("src/core/gc_hooks.h");
    CHECK(!mb.empty(), "read mutation boundary");
    CHECK(!hooks.empty(), "read gc_hooks");
    CHECK(mb.find("#2120") != std::string::npos, "AC3: #2120 pipeline retained");
    CHECK(mb.find("clear_gc_defer_for_evaluator") != std::string::npos,
          "AC3: #2086-style clear retained");
    CHECK(mb.find("release_mutation_hold_defer") != std::string::npos, "AC3: hold release");
    CHECK(mb.find("#2211") != std::string::npos || mb.find("Issue #2211") != std::string::npos,
          "AC3: cites #2211");
    CHECK(mb.find("mutation_boundary_residual_defer_total") != std::string::npos,
          "AC3: residual metric wired in dtor");
    CHECK(mb.find("AURA_HARD_RESIDUAL_DEFER") != std::string::npos, "AC3: hard env opt-in");
    CHECK(hooks.find("defer_reasons_snapshot") != std::string::npos, "AC3: #2088 snapshot API");
    CHECK(hooks.find("GcDeferReason") != std::string::npos, "AC3: #2088 reason enum");

    // Runtime smoke: phase counters still advance (2120 pipeline).
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto p3_0 = m->outermost_exit_phase3_gc_defer_total.load(std::memory_order_relaxed);
    const auto p5_0 = m->outermost_exit_phase5_unlock_total.load(std::memory_order_relaxed);
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
    }
    CHECK(m->outermost_exit_phase3_gc_defer_total.load(std::memory_order_relaxed) >= p3_0 + 1,
          "AC3: phase3 still advances");
    CHECK(m->outermost_exit_phase5_unlock_total.load(std::memory_order_relaxed) == p5_0 + 1,
          "AC3: phase5 still advances");
}

static void ac4_query_schema_2211() {
    std::println("\n--- AC4: query schema-2211 residual keys ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2211") == 2211, "schema-2211");
    CHECK(href(cs, "issue-2211") == 2211, "issue-2211");
    CHECK(href(cs, "residual-defer-assert-wired") == 1, "wired sentinel");
    CHECK(href(cs, "mutation-boundary-residual-defer-total") >= 0, "kebab residual key");
    CHECK(href(cs, "residual-defer-total") >= 0, "short residual alias");
    CHECK(href(cs, "mutation_boundary_residual_defer_total") >= 0, "snake residual alias");
    // Lineage fold into hold-stats (no new public primitive name).
    CHECK(href(cs, "schema-2120") == 2120, "schema-2120 retained");
    CHECK(href(cs, "outermost-exit-order-wired") == 1, "2120 wired retained");

    auto qsrc = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(qsrc.find("schema-2211") != std::string::npos, "query cites schema-2211");
    CHECK(qsrc.find("mutation-boundary-residual-defer-total") != std::string::npos,
          "query residual key");
    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(fields.find("mutation_boundary_residual_defer_total") != std::string::npos,
          "metrics field declared");
    auto oh = read_file("src/compiler/observability_metrics.h");
    CHECK(oh.find("mutation_boundary_residual_defer_total") != std::string::npos,
          "observability field declared");
}

} // namespace

int main() {
    std::println("=== Issue #2211: residual GC-defer assert at outermost Guard exit ===");
    ac1_normal_success_no_residual();
    ac2_inject_residual_bumps_and_clears();
    ac3_lineage_retained();
    ac4_query_schema_2211();

    std::println("\n=== test_residual_gc_defer_assert_2211: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
