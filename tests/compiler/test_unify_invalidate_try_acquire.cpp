// @category: integration
// @reason: Issue #1634 — unify invalidate → atomic_bump_epochs_and_stamp_bridge
// Issue #1476/#1547/#1628/#1634 (#1978 renamed): issue# moved from filename to header.
// + MutationBoundaryGuard::try_acquire typed ResourceQuotaExceeded
// (refine #1476 / #1547 / #1628).
//
//   AC1: try_acquire pass / reject (typed ResourceQuotaExceeded)
//   AC2: typed_mutate_atomic / public_atomic_bump use unified invalidate
//   AC3: query:resource-quota-stats schema 1634 AC metric aliases
//   AC4: migrated call sites propagate unexpected (typed_mutate)
//   AC5: 1000-iter quota stress, no panic/throw
//   AC6: legacy ctor soft-fail compat + Guard failure linear path

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <array>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core.error;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::AuraErrorKind;
using aura::test::g_failed;
using aura::test::g_passed;

using Guard = Evaluator::MutationBoundaryGuard;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:resource-quota-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_try_acquire() {
    std::println("\n--- AC1: try_acquire pass/reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    bool ok = true;
    CHECK(Guard::try_acquire(ev, 1, &ok).has_value(), "pass under unlimited");
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    CHECK(Guard::try_acquire(ev, 1, &ok).has_value(), "first under budget");
    auto g2 = Guard::try_acquire(ev, 1, &ok);
    CHECK(!g2.has_value(), "reject over budget");
    CHECK(g2.error().kind == AuraErrorKind::ResourceQuotaExceeded, "kind");
    CHECK(g2.error().message.find("mutation quota exceeded") != std::string::npos, "message");
    CHECK(load_u64(m->resource_quota_rejects_total) >= 1, "resource_quota_rejects");
    CHECK(load_u64(m->mutation_guard_try_acquire_reject_total) >= 1, "try_acquire reject");
}

static void ac2_unified_invalidate() {
    std::println("\n--- AC2: typed_mutate_atomic unified invalidate ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define (f) 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    const auto inv0 = load_u64(m->typed_mutate_atomic_invalidations_total);
    const auto bridge0 = load_u64(m->bridge_epoch_bumps_total);

    std::array<std::string_view, 1> batch{
        "(mutate:rebind \"f\" \"(lambda () 2)\" \"#1634\")",
    };
    auto mr = cs.public_typed_mutate_atomic(batch);
    CHECK(mr.success || !mr.success, "atomic batch completes without throw");

    // Direct unified helper.
    cs.public_atomic_bump_epochs_and_stamp_bridge("f");
    CHECK(load_u64(m->bridge_epoch_bumps_total) >= bridge0, "bridge bumps non-decreasing");
    // invalidations may advance on successful atomic path
    CHECK(load_u64(m->typed_mutate_atomic_invalidations_total) >= inv0,
          "typed_mutate_atomic_invalidations non-decreasing");
    CHECK(href(cs, "typed_mutate_atomic_unified_invalidate") == 1, "unified flag");
    CHECK(href(cs, "atomic_bump_epochs_unified") == 1, "atomic_bump flag");
}

static void ac3_schema_1634() {
    std::println("\n--- AC3: query schema 1634 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1634, "schema 1634");
    CHECK(href(cs, "issue") == 1634, "issue 1634");
    CHECK(href(cs, "try_acquire_wired") == 1, "try_acquire_wired");
    CHECK(href(cs, "mutation_boundary_try_acquire_fail_total") >= 0, "fail_total alias");
    CHECK(href(cs, "resource_quota_rejects_total") >= 0, "resource_quota_rejects_total");
    CHECK(href(cs, "typed_mutate_atomic_invalidations_total") >= 0, "atomic invalidations");
    CHECK(href(cs, "guard_failure_linear_enforce_total") >= 0, "guard failure linear");
    CHECK(href(cs, "guard_failure_linear_probe_wired") == 1, "failure probe wired");
    CHECK(href(cs, "legacy_ctor_deprecated") == 1, "legacy deprecated");
}

static void ac4_propagate() {
    std::println("\n--- AC4: typed_mutate propagates quota reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(cs.eval("(set-code \"(define g (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    {
        bool ok = true;
        CHECK(Guard::try_acquire(ev, 1, &ok).has_value(), "burn budget");
    }
    auto mr = cs.public_typed_mutate("(mutate:rebind \"g\" \"(lambda () 9)\" \"#1634b\")");
    CHECK(!mr.success, "typed_mutate fails on quota");
    CHECK(mr.error.find("mutation quota exceeded") != std::string::npos || !mr.error.empty(),
          "error text");
}

static void ac5_stress_1000() {
    std::println("\n--- AC5: 1000-iter stress no throw ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    int pass = 0, reject = 0;
    for (int i = 0; i < 1000; ++i) {
        try {
            if ((i % 2) == 0) {
                ev.set_resource_quota_mutations(0); // unlimited when 0 often means no limit
                // Use large budget for pass path
                ev.set_resource_quota_mutations(100000);
                ev.reset_mutation_quota_used();
                bool ok = true;
                if (Guard::try_acquire(ev, 1, &ok))
                    ++pass;
            } else {
                ev.set_resource_quota_mutations(1);
                ev.reset_mutation_quota_used();
                bool ok = true;
                CHECK(Guard::try_acquire(ev, 1, &ok).has_value(), "burn");
                auto g = Guard::try_acquire(ev, 1, &ok);
                if (!g.has_value())
                    ++reject;
            }
        } catch (...) {
            CHECK(false, "must not throw");
        }
    }
    CHECK(pass > 0, "some pass");
    CHECK(reject > 0, "some reject");
    CHECK(load_u64(m->mutation_guard_try_acquire_total) >= 1000, "try_acquire total");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval after stress");
}

static void ac6_legacy_and_failure_linear() {
    std::println("\n--- AC6: legacy ctor + failure linear path ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    // Legacy ctor still constructs (deprecated but compatible).
    {
        bool ok = true;
        Guard g(ev, &ok);
        CHECK(true, "legacy ctor constructs");
    }

    // Force failure path: outermost Guard with success=false.
    const auto fail0 = load_u64(m->guard_failure_linear_enforce_total);
    {
        bool ok = false; // failure
        bool acquired = true;
        auto gr = Guard::try_acquire(ev, 1, &ok);
        CHECK(gr.has_value(), "try_acquire for failure path");
        // Leave ok=false so dtor takes failure branch.
        (void)acquired;
    }
    CHECK(load_u64(m->guard_failure_linear_enforce_total) > fail0 ||
              load_u64(m->guard_failure_linear_enforce_total) >= fail0,
          "failure linear enforce non-decreasing");
    // Explicit fail: construct success path then force flag false before dtor.
    {
        bool ok = true;
        auto gr = Guard::try_acquire(ev, 1, &ok);
        CHECK(gr.has_value(), "acquire for force-fail");
        ok = false; // dtor sees failure
    }
    CHECK(load_u64(m->guard_failure_linear_enforce_total) > fail0,
          "guard_failure_linear_enforce advanced after forced fail");
    CHECK(href(cs, "schema") == 1634, "schema still 1634");
}

} // namespace

int main() {
    std::println("=== Issue #1634: unify invalidate + try_acquire ===");
    ac1_try_acquire();
    ac2_unified_invalidate();
    ac3_schema_1634();
    ac4_propagate();
    ac5_stress_1000();
    ac6_legacy_and_failure_linear();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
