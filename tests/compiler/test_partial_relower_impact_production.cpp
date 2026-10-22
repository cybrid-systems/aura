// @category: unit
// @reason: Issue #3310 — Production fail-closed on unknown ImpactScope
// (refine #3034 / #3068 / #3097). impact_upper_bound_for_entry_
// returns 0 when the workspace flat / pool is missing or the define
// root is not found; that 0 is *unknown* impact, not *empty* impact.
// Under production defaults, an unknown impact on a non-zero dirty
// window must fail-closed to full (silent under-cascade on a partial
// peel would under-mark cross-fn callee edges that only the hybrid
// DepGraph / ImpactScope walk would have seen). Soft / Off keeps
// zero-cost threshold partial when ub==0 (existing contract). The
// map-empty sentinel -1 path through should_partial_relower_impact_checked
// is untouched (AC3 — sentinel still forces full via the existing
// impact_ub > dirty_count branch).
//
//   AC1: production + dirty_n > 0 + ub == 0 → false (force full).
//        Soft/Off + dirty_n > 0 + ub == 0 → matches existing
//        should_partial_relower (zero-cost contract preserved).
//   AC2: production + dirty_n > 0 + ub <= dirty_n → matches existing
//        should_partial_relower (no #3034 / #2206 regression).
//   AC3: production + dirty_n > 0 + sentinel -1 (map empty / desync)
//        → false (existing path; helper delegates unchanged).
//   AC4: dirty_n == 0 → false in all branches (clean window).
//   AC5: production_consult reflects
//        production_defaults_active() || AuditStrategy::Full; under
//        apply_dev_audit_defaults() + Sampled, production_consult is
//        false (existing helper path runs, Soft/Off zero-cost).
//   AC6: Source-cite — service.ixx relower_dirty_defines_from_workspace
//        partial branch calls should_partial_relower_impact_checked_prod
//        with production_consult derived from typed_audit; no other
//        production-fail-close gates removed.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.ir_cache_pure;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::should_partial_relower_impact_checked;
using aura::compiler::should_partial_relower_impact_checked_prod;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::get_strategy;
using aura::compiler::typed_audit::production_defaults_active;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
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

// AC1 + AC4: production + ub == 0 + dirty_n > 0 → false (force full).
// Soft + ub == 0 + dirty_n > 0 → matches existing helper (zero-cost).
static void ac1_production_unknown_impact_fail_closed() {
    // Production + 3 dirty + ub == 0 → false.
    CHECK(!should_partial_relower_impact_checked_prod(3, 0, /*production=*/true),
          "AC1: production + dirty_n=3 + ub=0 → false (force full)");
    // Production + 1 dirty + ub == 0 → false.
    CHECK(!should_partial_relower_impact_checked_prod(1, 0, /*production=*/true),
          "AC1: production + dirty_n=1 + ub=0 → false (force full)");
    // Production + huge dirty + ub == 0 → false.
    CHECK(!should_partial_relower_impact_checked_prod(1024, 0, /*production=*/true),
          "AC1: production + dirty_n=1024 + ub=0 → false (force full)");
    // Soft + 3 dirty + ub == 0 → matches existing helper
    // (should_partial_relower_impact_checked(3, 0) — only early-exit
    //  on dirty_count == 0 fires, returns should_partial_relower(3)).
    const bool soft_ub0 = should_partial_relower_impact_checked_prod(3, 0, false);
    const bool soft_existing = should_partial_relower_impact_checked(3, 0);
    CHECK(soft_ub0 == soft_existing, "AC1+AC4: Soft + dirty_n=3 + ub=0 → matches existing helper "
                                     "(zero-cost contract preserved)");
    CHECK(soft_ub0, "AC4: Soft + dirty_n=3 + ub=0 → true (partial allowed)");
    // Soft + 1 dirty + ub == 0 → true (threshold met).
    CHECK(should_partial_relower_impact_checked_prod(1, 0, false),
          "AC4: Soft + dirty_n=1 + ub=0 → true (partial allowed)");
}

// AC2: production + computable ub with ub <= dirty_n → matches existing
// helper (no #3034 / #2206 regression). Boundary cases.
static void ac2_production_computable_ub_matches_existing() {
    // ub == dirty_n → matches existing (equal threshold is allowed).
    CHECK(should_partial_relower_impact_checked_prod(3, 3, true) ==
              should_partial_relower_impact_checked(3, 3),
          "AC2: production + dirty_n=3 + ub=3 → matches existing");
    // ub < dirty_n → matches existing (strict under-count is fine).
    CHECK(should_partial_relower_impact_checked_prod(3, 1, true) ==
              should_partial_relower_impact_checked(3, 1),
          "AC2: production + dirty_n=3 + ub=1 → matches existing");
    // ub == 0 + production true → false (the new fail-closed), even
    // when existing helper would have returned true.
    CHECK(!should_partial_relower_impact_checked_prod(3, 0, true),
          "AC2: production + dirty_n=3 + ub=0 → false (new fail-closed)");
    // ub == 0 + production false → true (Soft zero-cost preserved).
    CHECK(should_partial_relower_impact_checked_prod(3, 0, false),
          "AC2: Soft + dirty_n=3 + ub=0 → true (zero-cost preserved)");
}

// AC3: production + sentinel -1 (map empty / desync) → false (existing
// path through should_partial_relower_impact_checked unchanged).
static void ac3_production_sentinel_minus_one() {
    // Sentinel -1 cast to size_t is huge; the existing
    // should_partial_relower_impact_checked branch
    // `impact_upper_bound > dirty_count` fires → false. The new
    // production check (impact_ub == 0) does NOT fire on -1.
    const std::size_t sentinel = static_cast<std::size_t>(-1); // map empty / desync sentinel
    CHECK(!should_partial_relower_impact_checked_prod(3, sentinel, true),
          "AC3: production + dirty_n=3 + ub=SENTINEL → false "
          "(existing path, sentinel force-full preserved)");
    CHECK(!should_partial_relower_impact_checked_prod(3, sentinel, false),
          "AC3: Soft + dirty_n=3 + ub=SENTINEL → false "
          "(existing path unchanged for Soft too)");
    // Confirm new fail-closed does NOT fire on sentinel: prod + dirty +
    // sentinel → false via existing branch, not via the new
    // production && ub==0 branch.
    CHECK(!should_partial_relower_impact_checked_prod(3, 4, true),
          "AC3: production + dirty_n=3 + ub=4 → false (existing branch, "
          "ub > dirty_n gate)");
}

// AC4: dirty_n == 0 → false in all branches (clean window, zero-cost
// early-exit).
static void ac4_clean_window_zero_cost() {
    CHECK(!should_partial_relower_impact_checked_prod(0, 0, true),
          "AC4: clean window + production + ub=0 → false");
    CHECK(!should_partial_relower_impact_checked_prod(0, 0, false),
          "AC4: clean window + Soft + ub=0 → false");
    CHECK(!should_partial_relower_impact_checked_prod(0, 5, true),
          "AC4: clean window + production + ub=5 → false");
    CHECK(!should_partial_relower_impact_checked_prod(0, 5, false),
          "AC4: clean window + Soft + ub=5 → false");
    CHECK(!should_partial_relower_impact_checked_prod(0, static_cast<std::size_t>(-1), true),
          "AC4: clean window + production + ub=SENTINEL → false");
}

// AC5: production_consult reflects production_defaults_active() ||
// AuditStrategy::Full; under apply_dev_audit_defaults() + Sampled,
// production_consult is false (Soft/Off zero-cost contract).
static void ac5_production_consult_reflects_strategy() {
    apply_production_audit_defaults();
    const bool production_prod =
        production_defaults_active() || get_strategy() == AuditStrategy::Full;
    CHECK(production_prod, "AC5: apply_production_audit_defaults → production_consult true");
    apply_dev_audit_defaults();
    const bool production_dev =
        production_defaults_active() || get_strategy() == AuditStrategy::Full;
    CHECK(!production_dev, "AC5: apply_dev_audit_defaults → production_consult false "
                           "(Sampled + dev_audit_opt_in=1, Soft zero-cost)");
    apply_dev_audit_defaults();
}

static void ac3691_quote_lambda_impact_uses_prod_helper() {
    std::println("\n--- #3691: quote/lambda + invalidate cascade use impact_checked_prod ---");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    const auto sixx = read_file("src/compiler/service.ixx");
    CHECK(dirty.find("Issue #3691") != std::string::npos, "3691: dirty cites #3691");
    CHECK(dirty.find("should_partial_relower_impact_checked_prod") != std::string::npos,
          "3691 AC1: quote/lambda + cascade use _prod");
    const auto n3691 = dirty.find("Issue #3691: use the");
    const auto n3691_win = n3691 == std::string::npos ? std::string{} : dirty.substr(n3691, 2200);
    CHECK(n3691_win.find("should_partial_relower_impact_checked_prod") != std::string::npos,
          "3691 AC1: invalidate_bridge_with_impact uses _prod");
    CHECK(n3691_win.find("mark_all_blocks_dirty()") != std::string::npos,
          "3691 AC1: production fail-closed marks all blocks");
    // Match call sites (trailing '(' / '()'), not the comment that names
    // apply_impact_scope_dirty before the mark_all statement.
    const auto mark = n3691_win.find("mark_all_blocks_dirty()");
    const auto apply = n3691_win.find("apply_impact_scope_dirty(");
    CHECK(mark != std::string::npos && apply != std::string::npos && mark < apply,
          "3691 AC1: mark_all before subset apply (no subset peel after fail-closed)");
    CHECK(n3691_win.find("production_consult") != std::string::npos,
          "3691 AC3: Soft/Off consult is production_consult");
    CHECK(dirty.find("should_partial_relower_impact_checked_prod(dirty_n, impact_ub") !=
              std::string::npos,
          "3691 AC1: try_partial_invalidate_relower uses _prod");
    CHECK(sixx.find("should_partial_relower_impact_checked_prod(dirty_n, impact_ub") !=
              std::string::npos,
          "3691 AC2: eval-path impact_checked_prod unchanged");
    CHECK(dirty.find("schema-3691") == std::string::npos, "3691 AC5: no new query key");
    CHECK(read_file("tests/compiler/test_issue_3691.cpp").empty(), "3691: no test_issue_3691.cpp");
    CHECK(read_file("docs/design/3691-quote-lambda-impact-prod.md").empty(),
          "3691: no docs/design/");

    apply_dev_audit_defaults();
    CompilerService cs;
    cs.evaluator().set_effect_sandbox_mode(0);
    CHECK(cs.eval("(+ 1 1)").has_value(), "3691 soak: warm");
    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "3691 soak: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3691 soak: eval");
    apply_production_audit_defaults();
    auto mut = cs.eval("(mutate:set-body \"f\" \"(lambda () 9)\" \"#3691\")");
    CHECK(mut.has_value() && !is_error(*mut), "3691 soak: mutate lambda body");
    auto src_after_mut = cs.eval("(current-source :workspace)");
    CHECK(cs.eval("(eval-current)").has_value(), "3691 soak: re-eval");
    auto r = cs.eval("(f)");
    std::string got = "3691 soak: mutated lambda";
    if (!r)
        got += " (no value: " + r.error().format() + ")";
    else if (is_error(*r))
        got += " (error value)";
    else if (!is_int(*r))
        got += " (not int)";
    else
        got += " (got " + std::to_string(as_int(*r)) + ")";
    auto append_src = [&](const char* tag, const auto& src) {
        if (src && is_string(*src)) {
            const auto idx = as_string_idx(*src);
            const auto heap = cs.evaluator().string_heap();
            if (idx < heap.size())
                got += std::string(" ") + tag + "=" + std::string(heap[idx]);
        }
    };
    append_src("pre", src_after_mut);
    if (auto src = cs.eval("(current-source :workspace)"); src)
        append_src("src", src);
    CHECK(r && is_int(*r) && as_int(*r) == 9, got);
    apply_dev_audit_defaults();
    CHECK(cs.eval("(f)").has_value(), "3691 AC3: Soft still evals");
}

// Issue #4422: production cache_define_prefer_partial and the sweep's
// impact-refused else branch must not commit a per-fn peel. Soft ub==0
// still enters the partial relower.
static void ac4422_unknown_impact_skips_per_fn_commit() {
    std::println("\n--- #4422: prefer-partial and sweep consult impact ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto pref = svc.find("EvalResult cache_define_prefer_partial(");
    const auto peel = svc.find("relower_only_dirty_blocks(name_str", pref);
    CHECK(pref != std::string::npos && peel != std::string::npos && pref < peel,
          "4422: prefer-partial peel call located");
    if (pref != std::string::npos && peel != std::string::npos && pref < peel) {
        const auto mid = svc.substr(pref, peel - pref);
        CHECK(mid.find("Issue #4422") != std::string::npos, "4422: prefer-partial cites #4422");
        CHECK(mid.find("should_partial_relower_impact_checked_prod") != std::string::npos,
              "4422: prefer-partial consults the existing helper");
        CHECK(mid.find("partial_forced_full_by_impact_total") != std::string::npos,
              "4422: prefer-partial reuses the impact counter");
    }
    CHECK(svc.find("commit_per_fn_partial && allow_partial_peel") != std::string::npos,
          "4422: per-fn arm is gated after impact refused");
    // Issue #4653 made the full arm's argument refusal-aware: the sweep now
    // passes commit_per_fn_partial_4653, defined as
    // !production_face_4653 && !impact_refused_partial. The #4422 contract
    // (no per-fn commit after the impact helper refuses) holds inside the
    // gate, so assert the gate and its #4422 term instead of the pre-#4653
    // literal argument.
    CHECK(svc.find("commit_per_fn_partial=*/commit_per_fn_partial_4653") != std::string::npos,
          "4422: sweep else passes the refusal-aware gate");
    const auto gate_def = svc.find("const bool commit_per_fn_partial_4653");
    CHECK(gate_def != std::string::npos &&
              svc.find("!impact_refused_partial", gate_def) != std::string::npos &&
              svc.find("!impact_refused_partial", gate_def) < gate_def + 256,
          "4422: sweep else does not commit per-fn after the helper refuses");
    CHECK(svc.find("query:4422") == std::string::npos, "4422: no new query key");
    CHECK(read_file("tests/compiler/test_issue_4422.cpp").empty(), "4422: no test_issue");
    CHECK(read_file("docs/design/4422-prefer-partial-impact.md").empty(), "4422: no docs/design");

    auto metrics = [](CompilerService& cs) -> aura::compiler::CompilerMetrics* {
        return static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
    };

    apply_production_audit_defaults();
    {
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        CHECK(cs.eval("(set-code \"(define (f4422 x) (+ x 1))\")").has_value(), "4422: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4422: eval");
        CHECK(cs.get_define_v2("f4422") != nullptr && !cs.get_define_v2("f4422")->irs.empty(),
              "4422: v2 entry");
        cs.evaluator().set_workspace_flat(nullptr);
        cs.evaluator().set_workspace_pool(nullptr);
        CHECK(cs.plant_body_only_dirty_for_test("f4422", false, false) >= 1, "4422: body dirty");
        auto* m = metrics(cs);
        const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        const auto per0 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
        const auto path0 = m->eval_path_relower_total.load(std::memory_order_relaxed);
        auto def = cs.define_function("(define (f4422 x) (+ x 9))");
        CHECK(def.has_value(), "4422: define_function under unknown impact");
        CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) > impact0,
              "4422: unknown impact bumps partial_forced_full_by_impact_total");
        CHECK(m->relower_per_function_called_count.load(std::memory_order_relaxed) == per0,
              "4422: unknown impact does not commit per-fn");
        CHECK(m->eval_path_relower_total.load(std::memory_order_relaxed) == path0,
              "4422: unknown impact does not enter the peel");
        const auto* after = cs.get_define_v2("f4422");
        if (after) {
            const int look = cs.lookup_define_v2("f4422", after->source_hash);
            CHECK(look != 0, "4422: unknown impact does not clean-hit the partial peel");
        }
    }

    apply_dev_audit_defaults();
    {
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        CHECK(cs.eval("(set-code \"(define (s4422 x) (+ x 1))\")").has_value(),
              "4422: Soft set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4422: Soft eval");
        CHECK(cs.get_define_v2("s4422") != nullptr, "4422: Soft v2 entry");
        cs.evaluator().set_workspace_flat(nullptr);
        cs.evaluator().set_workspace_pool(nullptr);
        CHECK(cs.plant_body_only_dirty_for_test("s4422", false, false) >= 1,
              "4422: Soft body dirty");
        auto* m = metrics(cs);
        const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        const auto path0 = m->eval_path_relower_total.load(std::memory_order_relaxed);
        auto def = cs.define_function("(define (s4422 x) (+ x 3))");
        CHECK(def.has_value(), "4422: Soft define_function");
        CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) == impact0,
              "4422: Soft ub==0 does not force-full");
        CHECK(m->eval_path_relower_total.load(std::memory_order_relaxed) > path0,
              "4422: Soft still enters the partial relower");
    }

    apply_production_audit_defaults();
    {
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        CHECK(cs.eval("(set-code \"(define (w4422 x) (+ x 1))\")").has_value(),
              "4422: sweep set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4422: sweep eval");
        CHECK(cs.plant_body_only_dirty_for_test("w4422", false, false) >= 1, "4422: sweep dirty");
        auto* m = metrics(cs);
        const auto per0 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
        const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
        (void)cs.public_relower_dirty_defines_from_workspace();
        CHECK(m->relower_per_function_called_count.load(std::memory_order_relaxed) == per0,
              "4422: sweep does not commit per-fn after impact refuses");
        CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) > full0,
              "4422: sweep reaches the full store_define_v2 closeout");
    }
    apply_dev_audit_defaults();
}

} // namespace

int run_test_issue_3310() {
    std::print("[test_issue_3310] running 5 ACs (production fail-closed on "
               "unknown ImpactScope)\n");

    ac1_production_unknown_impact_fail_closed();
    ac2_production_computable_ub_matches_existing();
    ac3_production_sentinel_minus_one();
    ac4_clean_window_zero_cost();
    ac5_production_consult_reflects_strategy();
    ac3691_quote_lambda_impact_uses_prod_helper();
    ac4422_unknown_impact_skips_per_fn_commit();

    std::print("[test_issue_3310] passed={} failed={}\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_issue_3310();
}
#endif
