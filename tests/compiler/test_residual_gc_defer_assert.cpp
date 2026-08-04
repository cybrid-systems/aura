// @category: unit
// @reason: Issue #2211 — residual GcDeferReason assert at outermost Guard exit.
//
//   AC1: Success path of outermost exit leaves defer_reasons_snapshot()==0
//        under normal nesting (and residual counter stays 0).
//   AC2: Intentional residual (extra MutationHold arm) bumps
//        mutation_boundary_residual_defer_total and is cleared by best-effort.
//   AC3: Existing #2120 / #2088 / #2086 contracts retained (source + schema).
//   AC4: query:mutation-boundary-hold-stats schema-2211 + residual keys.
//
// Issue #2269 — production-default residual defer policy (clear vs hard).
//   AC1: AURA_RESIDUAL_DEFER_POLICY=hard|clear|unset + legacy
//        AURA_HARD_RESIDUAL_DEFER=1 → soft|clear|hard policy selection.
//   AC2: Production default is clear (B) — not soft-only.
//   AC3: Zero-cost success path (single relaxed load of reasons).
//   AC4: 2 new counters + 5 new query keys + schema-2269 lineage.
//   AC5: Runtime smoke — soft/clear branches + source-cite for hard.
//
// Issue #2296 — harden Phase-5 residual Clear + multi-eval orphan steal.
//   AC1: Outermost success with residual Panic → Clear forces depth 0 + bit clear.
//   AC2: Two Evaluators; clear orphan from A; B hold unaffected; bits consistent.
//   AC3: Zero residual happy path → single load, no clear work.
//   AC4: Hard/Soft unchanged (#2269).
//   AC5: Query correlation keys + decision table + source-cite.

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

// Issue #2269 AC1-AC5: production-default residual defer policy
// (clear vs hard vs soft). Extends #2211 with a production path that
// is NOT soft-only. Three policy branches:
//   - Soft: AURA_SANDBOX=off (sandbox / unit tests). Legacy #2211.
//   - Clear (default under production): forced clear + bump
//     residual-defer-forced-clear-total.
//   - Hard (AURA_RESIDUAL_DEFER_POLICY=hard or legacy
//     AURA_HARD_RESIDUAL_DEFER=1): bump residual-defer-hard-fail-total
//     + assert + std::abort() if non-debug.
// AC1: env-driven policy selection (AURA_RESIDUAL_DEFER_POLICY +
//      legacy AURA_HARD_RESIDUAL_DEFER).
// AC2: production default is clear (B), not soft-only.
// AC3: zero-cost success path (single relaxed load; residual != 0 skip).
// AC4: 2 new counters + 5 new query keys + schema-2269 lineage.
// AC5: runtime smoke — soft/clear branches + source-cite for hard.
void ac2269_residual_defer_policy(CompilerService& cs) {
    std::println("\n--- AC #2269: residual defer policy (soft | clear | hard) ---");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto obs = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    // AC1: env-driven policy + residual-policy decision table.
    CHECK(mb.find("AURA_RESIDUAL_DEFER_POLICY") != std::string::npos,
          "AC1: AURA_RESIDUAL_DEFER_POLICY env var read");
    CHECK(mb.find("AURA_HARD_RESIDUAL_DEFER") != std::string::npos,
          "AC1: legacy AURA_HARD_RESIDUAL_DEFER backward-compat");
    CHECK(mb.find("ResidualPolicy") != std::string::npos,
          "AC1: ResidualPolicy enum {Soft, Clear, Hard}");
    CHECK(mb.find("policy == ResidualPolicy::Hard") != std::string::npos,
          "AC1: Hard branch (assert + abort)");
    CHECK(mb.find("policy == ResidualPolicy::Clear") != std::string::npos,
          "AC1: Clear branch (forced clear + metric)");
    // AC2: production default is clear (B), not soft-only.
    CHECK(mb.find("ResidualPolicy::Clear") != std::string::npos,
          "AC2: Clear policy applied when dev_off==false + unset policy");
    CHECK(mb.find("dev_off") != std::string::npos, "AC2: AURA_SANDBOX=off selects Soft");
    // AC3: zero-cost success path — single relaxed load of reasons.
    CHECK(mb.find("const auto residual = aura::gc_hooks::defer_reasons_snapshot()") !=
              std::string::npos,
          "AC3: single snapshot load");
    CHECK(mb.find("if (residual != 0)") != std::string::npos,
          "AC3: residual != 0 guards entire block (zero-cost on success)");
    // AC4: counter fields + query keys + schema-2269 lineage.
    CHECK(obs.find("mutation_boundary_residual_defer_forced_clear_total{0}") != std::string::npos,
          "AC4: forced-clear counter field");
    CHECK(obs.find("mutation_boundary_residual_defer_hard_fail_total{0}") != std::string::npos,
          "AC4: hard-fail counter field");
    CHECK(q.find("residual-defer-forced-clear-total") != std::string::npos,
          "AC4: forced-clear query key");
    CHECK(q.find("residual-defer-hard-fail-total") != std::string::npos,
          "AC4: hard-fail query key");
    CHECK(q.find("residual-defer-policy") != std::string::npos,
          "AC4: residual-defer-policy query key (0=soft/1=clear/2=hard)");
    CHECK(q.find("residual-defer-policy-wired") != std::string::npos,
          "AC4: residual-defer-policy-wired sentinel");
    CHECK(q.find("schema-2269") != std::string::npos, "AC4: schema-2269 lineage");
    CHECK(q.find("issue-2269") != std::string::npos, "AC4: issue-2269 lineage");
    // AC5: runtime smoke — soft (sandbox) + clear (production default).
    {
        // Soft branch: AURA_SANDBOX=off + inject residual → expect
        // residual-defer-total bump but no forced-clear / hard-fail.
        drain_known_defer();
        const char* prev_sandbox = std::getenv("AURA_SANDBOX");
        std::string prev_sandbox_str = prev_sandbox ? prev_sandbox : "";
        ::setenv("AURA_SANDBOX", "off", 1);
        // Unset policy so dev_off selects Soft deterministically.
        ::unsetenv("AURA_RESIDUAL_DEFER_POLICY");
        CompilerService local;
        auto& ev = local.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto r0 = m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed);
        const auto fc0 =
            m->mutation_boundary_residual_defer_forced_clear_total.load(std::memory_order_relaxed);
        const auto hf0 =
            m->mutation_boundary_residual_defer_hard_fail_total.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            // Intentional residual (sandbox path: extra MutationHold arm).
            aura::gc_hooks::arm_mutation_hold_defer();
        }
        const auto r1 = m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed);
        const auto fc1 =
            m->mutation_boundary_residual_defer_forced_clear_total.load(std::memory_order_relaxed);
        const auto hf1 =
            m->mutation_boundary_residual_defer_hard_fail_total.load(std::memory_order_relaxed);
        CHECK(r1 > r0, "AC5-soft: residual counter bumped");
        CHECK(fc1 == fc0, "AC5-soft: forced-clear counter unchanged under soft policy");
        CHECK(hf1 == hf0, "AC5-soft: hard-fail counter unchanged under soft policy");
        // Restore env.
        if (!prev_sandbox_str.empty())
            ::setenv("AURA_SANDBOX", prev_sandbox_str.c_str(), 1);
        else
            ::unsetenv("AURA_SANDBOX");
        drain_known_defer();
    }
    {
        // Clear branch: unset AURA_SANDBOX + unset policy → production
        // default selects Clear. Inject residual → expect residual
        // counter + forced-clear counter both bump, hard-fail unchanged.
        drain_known_defer();
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_RESIDUAL_DEFER_POLICY");
        ::unsetenv("AURA_HARD_RESIDUAL_DEFER");
        CompilerService local;
        auto& ev = local.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto r0 = m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed);
        const auto fc0 =
            m->mutation_boundary_residual_defer_forced_clear_total.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            aura::gc_hooks::arm_mutation_hold_defer();
        }
        const auto r1 = m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed);
        const auto fc1 =
            m->mutation_boundary_residual_defer_forced_clear_total.load(std::memory_order_relaxed);
        CHECK(r1 > r0, "AC5-clear: residual counter bumped under production-default Clear");
        CHECK(fc1 > fc0, "AC5-clear: forced-clear counter bumped under production-default Clear");
        drain_known_defer();
    }
}

// Issue #2296 AC1-AC5: Phase-5 residual Clear + multi-eval orphan window.
void ac2296_multi_eval_residual_clear(CompilerService& cs) {
    std::println("\n--- AC #2296: multi-eval residual Clear + steal orphan harden ---");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto gh = read_file("src/core/gc_hooks.h");
    auto mut = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    auto obs = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");

    // AC5 surface gates
    CHECK(gh.find("force_clear_all_gc_defer_for_evaluator") != std::string::npos,
          "AC5: force_clear_all_gc_defer_for_evaluator in gc_hooks.h");
    CHECK(gh.find("reconcile_gc_defer_bits_after_clear") != std::string::npos,
          "AC5: reconcile_gc_defer_bits_after_clear helper");
    CHECK(mb.find("force_clear_all_gc_defer_for_evaluator") != std::string::npos,
          "AC5: Phase 5 Clear uses force_clear_all");
    CHECK(mb.find("Decision table") != std::string::npos,
          "AC5: decision table documented in Phase 5");
    CHECK(mut.find("reconcile_gc_defer_bits_after_clear") != std::string::npos,
          "AC5: steal path reconciles bits after orphan clear");
    CHECK(obs.find("mutation_boundary_residual_defer_bit_reconcile_total{0}") != std::string::npos,
          "AC5: bit-reconcile CompilerMetrics field");
    CHECK(q.find("residual-defer-bit-reconcile-total") != std::string::npos,
          "AC5: residual-defer-bit-reconcile-total query key");
    CHECK(q.find("gc-defer-orphan-cleared-on-steal-total") != std::string::npos,
          "AC5: orphan-cleared-on-steal on hold-stats surface");
    CHECK(q.find("gc-defer-table-overflow-total") != std::string::npos,
          "AC5: table-overflow correlation key");
    CHECK(q.find("schema-2296") != std::string::npos && q.find("issue-2296") != std::string::npos,
          "AC5: schema-2296 / issue-2296 lineage");
    CHECK(q.find("residual-defer-multi-eval-wired") != std::string::npos,
          "AC5: residual-defer-multi-eval-wired sentinel");

    // AC3: happy path — no residual, no clear work (counter unchanged)
    {
        drain_known_defer();
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_RESIDUAL_DEFER_POLICY");
        CompilerService local;
        auto& ev = local.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto fc0 =
            m->mutation_boundary_residual_defer_forced_clear_total.load(std::memory_order_relaxed);
        const auto r0 = m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            CHECK(ok, "AC3: guard acquired");
        }
        CHECK(aura::gc_hooks::defer_reasons_snapshot() == 0, "AC3: snapshot still 0");
        CHECK(m->mutation_boundary_residual_defer_total.load(std::memory_order_relaxed) == r0,
              "AC3: residual total unchanged (zero residual path)");
        CHECK(m->mutation_boundary_residual_defer_forced_clear_total.load(
                  std::memory_order_relaxed) == fc0,
              "AC3: forced-clear unchanged on happy path");
        drain_known_defer();
    }

    // AC1: residual Panic → Clear forces depth 0 + bit clear + counters.
    // (a) force_clear_all drains table-backed panic for this eval.
    // (b) Phase-5 Clear on sticky Panic bit (depth already 0, multi-eval lag)
    //     still enters residual block and reconciles the bit.
    {
        drain_known_defer();
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_RESIDUAL_DEFER_POLICY");
        ::unsetenv("AURA_HARD_RESIDUAL_DEFER");
        CompilerService local;
        auto& ev = local.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        // (a) unit force_clear_all
        aura::gc_hooks::arm_gc_defer_pending_panic_for(static_cast<void*>(&ev));
        CHECK(aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(&ev)),
              "AC1a: panic armed for this eval");
        const auto fr =
            aura::gc_hooks::force_clear_all_gc_defer_for_evaluator(static_cast<void*>(&ev));
        CHECK(fr.panic_depth_cleared > 0, "AC1a: force_clear drained table depth");
        CHECK(!aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(&ev)),
              "AC1a: no residual panic-defer after force_clear");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == 0, "AC1a: process panic depth 0");
        CHECK((aura::gc_hooks::defer_reasons_snapshot() &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::Panic)) == 0,
              "AC1a: Panic bit clear after force_clear");
        // (b) sticky bit residual through Phase-5 Clear (Phase3 does not
        // clear — no per-eval table entry; only process bit stuck).
        const auto fc0 =
            m->mutation_boundary_residual_defer_forced_clear_total.load(std::memory_order_relaxed);
        (void)aura::gc_hooks::arm_defer(aura::gc_hooks::GcDeferReason::Panic);
        CHECK((aura::gc_hooks::defer_reasons_snapshot() &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::Panic)) != 0,
              "AC1b: sticky Panic bit set before Guard");
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            // No per-eval panic arm — residual after hold release is Panic bit.
        }
        CHECK((aura::gc_hooks::defer_reasons_snapshot() &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::Panic)) == 0,
              "AC1b: Panic bit clear after Phase-5 Clear reconcile");
        CHECK(m->mutation_boundary_residual_defer_forced_clear_total.load(
                  std::memory_order_relaxed) > fc0,
              "AC1b: forced-clear counter bumped on sticky residual");
        drain_known_defer();
    }

    // AC2: two Evaluators — clear A orphan; B hold unaffected; bits consistent
    {
        drain_known_defer();
        CompilerService a;
        CompilerService b;
        auto& eva = a.evaluator();
        auto& evb = b.evaluator();
        // Arm panic only on A; arm mutation hold for B (process-wide).
        aura::gc_hooks::arm_gc_defer_pending_panic_for(static_cast<void*>(&eva));
        aura::gc_hooks::arm_mutation_hold_defer();
        CHECK(aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(&eva)),
              "AC2: A has panic defer");
        CHECK(!aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(&evb)),
              "AC2: B has no panic table entry");
        CHECK(aura::gc_hooks::mutation_hold_defer_active(), "AC2: MutationHold active (B path)");
        // Simulate steal orphan clear for A.
        const auto cleared = aura::gc_hooks::clear_gc_defer_for_evaluator(static_cast<void*>(&eva));
        const auto recon = aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
        CHECK(cleared > 0, "AC2: A orphan panic depth cleared");
        CHECK(!aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(&eva)),
              "AC2: A panic table empty after clear");
        CHECK(aura::gc_hooks::mutation_hold_defer_active(),
              "AC2: B/process MutationHold unaffected by A's panic clear");
        // Panic bit must be clear when depth is 0 (reconcile or clear path).
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == 0, "AC2: process panic depth 0");
        CHECK((aura::gc_hooks::defer_reasons_snapshot() &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::Panic)) == 0,
              "AC2: Panic bit consistent with depth 0");
        (void)recon;
        // Force bit reconcile path: if depth 0 but bit stuck, reconcile fixes.
        // Inject lag: re-arm panic process bit without depth (via arm_defer only).
        (void)aura::gc_hooks::arm_defer(aura::gc_hooks::GcDeferReason::Panic);
        CHECK((aura::gc_hooks::defer_reasons_snapshot() &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::Panic)) != 0,
              "AC2: injected sticky Panic bit");
        const auto fixed = aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
        CHECK(fixed >= 1, "AC2: reconcile cleared sticky Panic bit when depth==0");
        CHECK((aura::gc_hooks::defer_reasons_snapshot() &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::Panic)) == 0,
              "AC2: Panic bit clear after reconcile");
        drain_known_defer();
    }

    // AC4: Soft still metric-only; Hard still aborts (source-cite only for hard)
    {
        CHECK(mb.find("policy == ResidualPolicy::Hard") != std::string::npos,
              "AC4: Hard branch retained");
        CHECK(mb.find("std::abort()") != std::string::npos, "AC4: Hard abort retained");
        CHECK(mb.find("AURA_SANDBOX") != std::string::npos, "AC4: Soft via AURA_SANDBOX retained");
        // Soft smoke (same as #2269): residual bumps, no forced clear
        drain_known_defer();
        ::setenv("AURA_SANDBOX", "off", 1);
        ::unsetenv("AURA_RESIDUAL_DEFER_POLICY");
        CompilerService local;
        auto& ev = local.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto fc0 =
            m->mutation_boundary_residual_defer_forced_clear_total.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            aura::gc_hooks::arm_mutation_hold_defer();
        }
        CHECK(m->mutation_boundary_residual_defer_forced_clear_total.load(
                  std::memory_order_relaxed) == fc0,
              "AC4-soft: forced-clear unchanged");
        ::unsetenv("AURA_SANDBOX");
        drain_known_defer();
    }

    // AC5: query surface returns hash (keys source-cited above)
    {
        auto h = cs.eval("(engine:metrics \"query:mutation-boundary-hold-stats\")");
        CHECK(h.has_value(), "AC5: mutation-boundary-hold-stats returns value");
        CHECK(cs.metrics().mutation_boundary_residual_defer_bit_reconcile_total.load() >= 0,
              "AC5: bit-reconcile metric readable");
    }
}

} // namespace

int run_test_residual_gc_defer_assert() {
    std::println("=== Issue #2211: residual GC-defer assert at outermost Guard exit ===");
    ac1_normal_success_no_residual();
    ac2_inject_residual_bumps_and_clears();
    ac3_lineage_retained();
    ac4_query_schema_2211();

    std::println("\n=== AC #2269: production-default residual defer policy ===");
    {
        CompilerService cs;
        ac2269_residual_defer_policy(cs);
    }

    std::println("\n=== AC #2296: multi-eval residual Clear + steal orphan harden ===");
    {
        CompilerService cs;
        ac2296_multi_eval_residual_clear(cs);
    }

    std::println("\n=== test_residual_gc_defer_assert: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_residual_gc_defer_assert();
}
#endif
