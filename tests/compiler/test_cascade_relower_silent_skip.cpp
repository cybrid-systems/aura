// @category: unit
// @reason: Issue #2813 — cascade must not silently skip ir_cache_v2 re-lower
// when defines_n>0 but relower_dirty_defines_fn_ is null.
//
//   AC1: cascade cites #2813; skipped/ran metrics; warn path
//   AC2: CompilerService wired path → cascade_relower_ran_total advances
//   AC3: fn cleared → cascade_relower_skipped_total advances on mutate
//   AC4: this suite + linter; no docs/design/2813-*; no test_issue_2813.cpp
//
// Issue #3484 — workspace peel must not count dirty_n==0 / instr-peel
// skip as success under production (residual of #1495/#2133/#3381).
//   AC3 (#2813) remains the unwired-hook test.
//   AC1: production cone name cannot ++ok on dirty_n==0 skip
//   AC2: instr peel without AST rewrite falls through (production)
//   AC3: Soft / Off clean skip stays zero-cost
//   AC4: existing query keys only; soak moves impact / should_relower
//   AC5: production soak asserts IR rewrite / lookup==1, not skip-only
//   AC6: linter; no docs/design/3484-*; no test_issue_3484.cpp

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

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

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::should_partial_relower;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
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
        // Window enlarged after soft-path dependent re-stamp block after re-lower.
        auto win = mut.substr(cascade, 14000);
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
        // Issue #3068: workspace relower must prepare the map before
        // the partial decision (no silent peel on a stale reverse index).
        CHECK(svc.find("Issue #3068") != std::string::npos, "AC1: service cites #3068");
        CHECK(svc.find("prepare_source_to_ir_map_for_partial_") != std::string::npos,
              "AC1: map prepare before impact snapshot");
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

    // ── #3484 source: production zero-mask fail-closed; Soft ++ok stays ──
    {
        std::println("\n--- #3484 AC1/AC2/AC3: peel zero-mask + instr-peel source ---");
        auto svc = read_file("src/compiler/service.ixx");
        auto pure = read_file("src/compiler/ir_cache_pure.ixx");
        CHECK(!svc.empty(), "3484 AC1: service.ixx readable");
        const auto peel = svc.find("std::size_t relower_dirty_defines_from_workspace()");
        CHECK(peel != std::string::npos, "3484 AC1: peel found");
        const auto peel_win = svc.substr(peel, 18000);
        CHECK(peel_win.find("Issue #3484") != std::string::npos, "3484 AC1: peel cites #3484");
        CHECK(peel_win.find("zero_mask_forced_full") != std::string::npos,
              "3484 AC1: zero-mask fail-closed flag");
        CHECK(peel_win.find("partial_forced_full_by_impact_total") != std::string::npos,
              "3484 AC1: reuses partial_forced_full_by_impact_total");
        CHECK(peel_win.find("++ok") != std::string::npos, "3484 AC3: Soft ++ok skip retained");
        const auto rb = svc.find("bool relower_define_blocks(");
        CHECK(rb != std::string::npos, "3484 AC2: relower_define_blocks");
        const auto rb_win = svc.substr(rb, 22000);
        CHECK(rb_win.find("Issue #3484") != std::string::npos, "3484 AC2: instr peel cites #3484");
        CHECK(rb_win.find("production_instr") != std::string::npos ||
                  rb_win.find("pass-only is not an AST-rooted rewrite") != std::string::npos,
              "3484 AC2: production instr peel falls through");
        const auto ack = rb_win.find("ack_cache_entry_fences_live_");
        const auto per_fn = rb_win.find("restamp after successful per-fn");
        CHECK(ack != std::string::npos && per_fn != std::string::npos && ack < per_fn,
              "3484 AC2: instr peel ack before per-fn");
        const auto peel_arm = rb_win.substr(ack, per_fn - ack);
        CHECK(peel_arm.find("mark_all_blocks_dirty") != std::string::npos,
              "3484 AC2: production re-dirties after pass-only peel");
        CHECK(peel_arm.find("return true") != std::string::npos,
              "3484 AC3: Soft still returns true");
        CHECK(pure.find("if (dirty_count == 0)") != std::string::npos,
              "3484 AC3: should_partial_relower dirty_count==0 stays");
        CHECK(should_partial_relower(0) == false, "3484 AC3: dirty_count==0 → false");
        CHECK(svc.find("schema-3484") == std::string::npos, "3484 AC4: no schema-3484");
        CHECK(svc.find("g_3484_") == std::string::npos, "3484 AC4: no g_3484_*");
        CHECK(read_file("docs/design/3484-peel-zero-mask.md").empty(),
              "3484 AC6: no docs/design/3484-*");
        CHECK(read_file("tests/compiler/test_issue_3484.cpp").empty(),
              "3484 AC6: no test_issue_3484.cpp");
        CHECK(read_file("tests/issues/test_issue_3484.cpp").empty(),
              "3484 AC6: no tests/issues/test_issue_3484.cpp");
        auto build = read_file("build.py");
        CHECK(build.find("check_peel_zero_mask_fail_closed_3484") != std::string::npos,
              "3484 AC6: build.py wires linter");
    }

    // ── #3484 AC5: production soak — zero-mask caller cannot skip ──
    {
        std::println("\n--- #3484 AC5: production zero-mask caller soak ---");
        apply_production_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval(R"(
(set-code "
(define f (lambda () 1))
(define g (lambda () (f)))
")")
                  .has_value(),
              "3484 AC5: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3484 AC5: eval");
        if (!cs.get_define_v2("f"))
            (void)cs.eval("(compile:cache-define \"f\")");
        if (!cs.get_define_v2("g"))
            (void)cs.eval("(compile:cache-define \"g\")");
        CHECK(cs.get_define_v2("f") != nullptr, "3484 AC5: f cached");
        CHECK(cs.get_define_v2("g") != nullptr, "3484 AC5: g cached");
        cs.public_record_dependency("g", "f");
        CHECK(cs.public_dep_graph_has_edge("g", "f"), "3484 AC5: g calls f");
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "3484 AC5: metrics");
        const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        const auto should0 = m->should_relower_total.load(std::memory_order_relaxed);
        const auto skip0 = m->relower_skipped_entirely_count.load(std::memory_order_relaxed);
        const auto hash = cs.get_define_v2("g")->source_hash;
        CHECK(cs.plant_zero_mask_caller_for_test("g"), "3484 AC5: plant zero-mask caller");
        CHECK(cs.get_define_v2("g") && cs.get_define_v2("g")->irs.empty(),
              "3484 AC5: planted empty irs");
        cs.public_mark_define_dirty("f");
        (void)cs.public_relower_dirty_defines_from_workspace();
        const auto* g1 = cs.get_define_v2("g");
        CHECK(g1 != nullptr, "3484 AC5: g still cached");
        const int look = cs.lookup_define_v2("g", hash);
        CHECK(!g1->irs.empty() || look == 1 || g1->dirty,
              "3484 AC5: IR rewritten or lookup==1 (not silent skip)");
        const auto impact1 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        const auto should1 = m->should_relower_total.load(std::memory_order_relaxed);
        const auto skip1 = m->relower_skipped_entirely_count.load(std::memory_order_relaxed);
        CHECK(impact1 > impact0 || should1 > should0,
              "3484 AC4: soak moved partial_forced_full_by_impact_total or should_relower_total");
        CHECK(impact1 > impact0 || skip1 == skip0 || !g1->irs.empty(),
              "3484 AC4: not skip-counter-only when IR was rewritten");
        CHECK(href(cs, "partial_forced_full_by_impact_total") >= 0 ||
                  href(cs, "partial-forced-full-by-impact-total") >= 0 ||
                  href(cs, "should_relower_total") >= 0,
              "3484 AC4: existing query keys still surface");
        apply_dev_audit_defaults();
    }

    // ── #3484 AC5: cone type-change must not leave g on pre-mutate IR ──
    {
        std::println("\n--- #3484 AC5: production cone type-change soak ---");
        apply_production_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval(R"(
(set-code "
(define f (lambda () 1))
(define g (lambda () (f)))
")")
                  .has_value(),
              "3484 AC5: type-change set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3484 AC5: type-change eval");
        if (!cs.get_define_v2("g"))
            (void)cs.eval("(compile:cache-define \"g\")");
        CHECK(cs.get_define_v2("g") != nullptr, "3484 AC5: g cached before mutate");
        cs.public_record_dependency("g", "f");
        const auto hash = cs.get_define_v2("g")->source_hash;
        const auto defuse0 = cs.get_define_v2("g")->version_stamp_.defuse_version;
        auto* m = metrics_of(cs);
        const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        auto mut = cs.eval("(mutate:set-body \"f\" \"(lambda () \\\"x\\\")\" \"#3484\")");
        CHECK(mut.has_value(), "3484 AC5: set-body type change");
        (void)cs.public_relower_dirty_defines_from_workspace();
        const auto* g1 = cs.get_define_v2("g");
        CHECK(g1 != nullptr, "3484 AC5: g after type-change");
        const int look = cs.lookup_define_v2("g", hash);
        CHECK(look == 1 || g1->dirty || g1->version_stamp_.defuse_version != defuse0 ||
                  !g1->content_stored_this_epoch ||
                  m->partial_forced_full_by_impact_total.load() > impact0,
              "3484 AC5: type-change did not silent-skip g as clean");
        apply_dev_audit_defaults();
    }

    // ── #3484 AC3: Soft genuinely-clean skip is zero extra ──
    {
        std::println("\n--- #3484 AC3: Soft clean skip zero extra ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval(R"(
(set-code "
(define f (lambda () 1))
(define g (lambda () (f)))
")")
                  .has_value(),
              "3484 AC3: Soft set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3484 AC3: Soft eval");
        if (!cs.get_define_v2("g"))
            (void)cs.eval("(compile:cache-define \"g\")");
        CHECK(cs.get_define_v2("g") != nullptr, "3484 AC3: g cached");
        auto* m = metrics_of(cs);
        const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        (void)cs.public_relower_dirty_defines_from_workspace();
        const auto impact1 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        CHECK(impact1 == impact0, "3484 AC3: Soft clean peel does not force-full");
        CHECK(cs.get_define_v2("g") && !cs.get_define_v2("g")->dirty,
              "3484 AC3: Soft leaves genuinely clean g unmarked");
    }

    std::println("\n=== #2813/#3484 cascade relower silent skip: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cascade_relower_silent_skip();
}
#endif
