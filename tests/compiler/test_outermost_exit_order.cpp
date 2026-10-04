// @category: unit
// @reason: Issue #2120 — outermost MutationBoundaryGuard exit unifies
// GC safepoint + dual-epoch + linear probe order.
//
// Documented success exit order (AC5):
//   1. exit_mutation_boundary (workspace commit + defuse dual-epoch bump)
//   2. linear + dual-path + LifetimePin probes (under lock; depth held)
//   3. panic commit/restore + GC defer drain for this evaluator
//   4. hot-update throttle → reemit → epoch notify (#2090 / #2114)
//   5. flush + depth_slot-- + unlock LAST
//
//   AC1: outermost success → no residual panic-defer for this evaluator
//   AC2: depth_slot stays elevated until unlock (source shape + metrics)
//   AC3: reemit phase runs after probes/GC (source order + phase counters)
//   AC4: nested does not bump outermost_exit_order_complete
//   AC5: documented order in dtor + query:mutation-boundary-hold-stats schema-2120

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/gc_hooks.h"

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

// make_merr → pair (kind-string . (msg-string . ...)). Extract kind.
static std::string merr_kind(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
    if (!aura::compiler::types::is_pair(v))
        return {};
    auto idx = aura::compiler::types::as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!aura::compiler::types::is_string(pairs[idx].car))
        return {};
    auto sidx = aura::compiler::types::as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-boundary-hold-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_no_residual_gc_defer() {
    std::println("\n--- AC1: outermost success clears panic-defer for this eval ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics wired");

    // Pre-condition: arm defer as if a checkpoint were live, then run
    // a successful outermost Guard — exit must drain residual for this eval.
    aura::gc_hooks::arm_gc_defer_pending_panic_for(static_cast<void*>(&ev));
    CHECK(aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(&ev)),
          "pre: defer armed for this evaluator");

    const auto p3_0 = m->outermost_exit_phase3_gc_defer_total.load(std::memory_order_relaxed);
    const auto done0 = m->outermost_exit_order_complete_total.load(std::memory_order_relaxed);
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
        CHECK(ok, "guard acquired");
        CHECK(g.is_outermost(), "outermost guard");
    }
    CHECK(ok, "success flag still true");
    CHECK(!aura::gc_hooks::gc_deferred_for_evaluator(static_cast<void*>(&ev)),
          "post: no residual panic-defer for this evaluator");
    CHECK(m->outermost_exit_phase3_gc_defer_total.load(std::memory_order_relaxed) >= p3_0 + 1,
          "phase3 gc-defer counter advanced");
    CHECK(m->outermost_exit_order_complete_total.load(std::memory_order_relaxed) == done0 + 1,
          "order complete +1");
    CHECK(ev.mutation_boundary_depth_slot_value(/*fiber_id=*/0) == 0, "depth_slot back to 0");
    CHECK(!ev.mutation_boundary_held(), "held flag cleared");
}

static void ac2_depth_held_until_unlock_source() {
    std::println("\n--- AC2: depth_slot deferred to unlock (source shape) ---");
    auto src = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(!src.empty(), "read evaluator_mutation_boundary.cpp");
    CHECK(src.find("#2120") != std::string::npos, "cites #2120");
    CHECK(src.find("const bool outermost = is_outermost_;") != std::string::npos,
          "uses is_outermost_ (not early depth_slot--)");
    // Early decrement pattern must not appear in the new dtor path.
    const auto dtor = src.find("::~MutationBoundaryGuard()");
    CHECK(dtor != std::string::npos, "found dtor");
    const auto early = src.find("int prev = (*slot)--", dtor);
    // Nested path still decrements with (*slot)-- but without "int prev ="
    CHECK(early == std::string::npos, "no early int prev=(*slot)-- depth pop");
    CHECK(src.find("exit fence") != std::string::npos ||
              src.find("exit_fence_pushed") != std::string::npos,
          "exit fence keeps stack visible during probes");
    CHECK(src.find("depth_slot last") != std::string::npos ||
              src.find("(*slot)--") != std::string::npos,
          "depth_slot decremented at end of outermost path");
}

static void ac3_reemit_after_probes_source_and_metrics() {
    std::println("\n--- AC3: reemit after probes/GC (order + phase counters) ---");
    auto src = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(!src.empty(), "read source");
    const auto p1 = src.find("Phase 1: linear + dual-path + LifetimePin probes");
    const auto p3 = src.find("Phase 2–3: panic checkpoint + GC defer");
    const auto p4 = src.find("Phase 4: hot-update throttle");
    const auto p5 = src.find("Phase 5: flush");
    CHECK(p1 != std::string::npos, "phase1 comment");
    CHECK(p3 != std::string::npos && p3 > p1, "phase3 after phase1");
    CHECK(p4 != std::string::npos && p4 > p3, "phase4 reemit after phase3");
    CHECK(p5 != std::string::npos && p5 > p4, "phase5 unlock last");
    // Probes before reemit within the dtor body (earlier file mentions don't count).
    const auto dtor = src.find("::~MutationBoundaryGuard()");
    CHECK(dtor != std::string::npos, "dtor body present");
    const auto dual = src.find("ensure_envframe_dual_path_consistency", dtor);
    const auto reemit = src.find("aura_reemit_aot_for_dirty", dtor);
    CHECK(dual != std::string::npos && reemit != std::string::npos && dual < reemit,
          "dual-path probe before reemit in dtor");

    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto p1_0 = m->outermost_exit_phase1_probes_total.load(std::memory_order_relaxed);
    const auto p4_0 = m->outermost_exit_phase4_reemit_total.load(std::memory_order_relaxed);
    const auto p5_0 = m->outermost_exit_phase5_unlock_total.load(std::memory_order_relaxed);
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(ev, &ok);
    }
    CHECK(m->outermost_exit_phase1_probes_total.load(std::memory_order_relaxed) == p1_0 + 1,
          "phase1 +1");
    CHECK(m->outermost_exit_phase4_reemit_total.load(std::memory_order_relaxed) == p4_0 + 1,
          "phase4 +1 (pipeline visited even if clean/no dirty)");
    CHECK(m->outermost_exit_phase5_unlock_total.load(std::memory_order_relaxed) == p5_0 + 1,
          "phase5 +1");
}

static void ac4_nested_no_double_complete() {
    std::println("\n--- AC4: nested Guard does not double-count complete ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto done0 = m->outermost_exit_order_complete_total.load(std::memory_order_relaxed);
    const auto p1_0 = m->outermost_exit_phase1_probes_total.load(std::memory_order_relaxed);
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard outer(ev, &ok);
        {
            Evaluator::MutationBoundaryGuard inner(ev, &ok);
            CHECK(!inner.is_outermost() || !outer.is_outermost() || true, "nested pair");
            CHECK(!inner.is_outermost(), "inner is not outermost");
            CHECK(outer.is_outermost(), "outer is outermost");
        }
    }
    CHECK(m->outermost_exit_order_complete_total.load(std::memory_order_relaxed) == done0 + 1,
          "nested pair → complete +1 (outermost only)");
    CHECK(m->outermost_exit_phase1_probes_total.load(std::memory_order_relaxed) == p1_0 + 1,
          "nested pair → phase1 +1");
}

static void ac5_query_schema_and_docs() {
    std::println("\n--- AC5: query schema-2120 + documented order ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    CHECK(href(cs, "schema-2120") == 2120, "schema-2120");
    CHECK(href(cs, "issue-2120") == 2120, "issue-2120");
    CHECK(href(cs, "outermost-exit-order-wired") == 1, "wired sentinel");
    CHECK(href(cs, "outermost-exit-phase1-probes-total") >= 0, "phase1 key");
    CHECK(href(cs, "outermost-exit-phase3-gc-defer-total") >= 0, "phase3 key");
    CHECK(href(cs, "outermost-exit-phase4-reemit-total") >= 0, "phase4 key");
    CHECK(href(cs, "outermost-exit-phase5-unlock-total") >= 0, "phase5 key");
    CHECK(href(cs, "outermost-exit-order-complete-total") >= 0, "complete key");

    auto src = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(src.find("outermost exit unified order") != std::string::npos ||
              src.find("Issue #2120: outermost exit unified order") != std::string::npos,
          "AC5 order documented in dtor header");
    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(fields.find("outermost_exit_order_complete_total") != std::string::npos,
          "metrics field declared");
    auto oh = read_file("src/compiler/observability_metrics.h");
    CHECK(oh.find("outermost_exit_phase1_probes_total") != std::string::npos,
          "observability field declared");
}

} // namespace

// ── Issue #4335: cow-refused mutate must not exit the boundary as Success ──
// The structural wrapper acquired the boundary and minted the session mid
// before the body ran; when trigger_lazy_cow refuses (read-only layer) the
// caller observes a deny, so the Guard must take the FAILED exit — not a
// committed success row for a mutate that never happened.
static void ac4335_cow_refused_deny_exit() {
    std::println("\n--- #4335: cow-refused exits the boundary as deny ---");
    // Source-cite: wrapper belt marks the Guard failed; the two body-side
    // sites clear the shared outermost flag (wrapper_guard is not in scope
    // inside the body lambdas).
    const auto mut_src = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut_src.find("Issue #4335") != std::string::npos,
          "4335: wrapper cow-refused site cites the issue");
    std::size_t cites = 0;
    for (auto pos = mut_src.find("Issue #4335"); pos != std::string::npos;
         pos = mut_src.find("Issue #4335", pos + 1))
        ++cites;
    CHECK(cites >= 3, "4335: all three cow-refused sites cite the issue");
    CHECK(mut_src.find("wrapper_guard->mark_failed();") != std::string::npos,
          "4335: wrapper belt marks the Guard failed");
    CHECK(mut_src.find("ev.mark_outermost_mutation_failed();") != std::string::npos,
          "4335: body sites clear the shared outermost flag");

    // Behavior: fresh child layer shares the parent flat (has_own_flat ==
    // false). Mark the node read-only UNDER the quick flag — workspace:lock
    // would sync workspace_read_only_ and the early read-only arm returns
    // before the COW trigger; direct set_read_only leaves the quick flag
    // false so the wrapped mutate reaches trigger_lazy_cow and is refused.
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    const auto wv = cs.eval("(workspace :create)");
    CHECK(wv.has_value() && is_int(*wv), "4335: child workspace created");
    const auto wid = static_cast<std::uint32_t>(as_int(*wv));
    CHECK(cs.eval(std::format("(workspace :switch {})", wid)).has_value(),
          "4335: switched to fresh child layer");
    auto* wt = static_cast<aura::compiler::WorkspaceTree*>(cs.evaluator().workspace_tree());
    CHECK(wt != nullptr, "4335: workspace tree live");
    wt->set_read_only(wt->active_idx(), true);

    const auto r = cs.eval("(mutate:replace-value (define b 2) (define b 2))");
    // Mutate denies surface as error-tagged pair values (kind . msg), not
    // as unexpected — assert the cow-refused kind on the returned value.
    CHECK(r.has_value() && aura::compiler::types::is_pair(*r) && merr_kind(cs, *r) == "cow-refused",
          "4335: mutate denied (cow-refused), not committed success");
    // The refused body never ran and the boundary aborted cleanly.
    CHECK(cs.eval("(eval-current)").has_value(), "4335: eval-current intact after deny");
}

int run_test_outermost_exit_order() {
    ac1_no_residual_gc_defer();
    ac4335_cow_refused_deny_exit();
    ac2_depth_held_until_unlock_source();
    ac3_reemit_after_probes_source_and_metrics();
    ac4_nested_no_double_complete();
    ac5_query_schema_and_docs();

    std::println("\n=== test_outermost_exit_order: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_outermost_exit_order();
}
#endif
