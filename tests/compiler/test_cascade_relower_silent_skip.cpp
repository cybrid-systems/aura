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
//
// Issue #4528 — a missing ir_cache_v2_ row must not let the orphan
// ir_cache_ callee body store as a clean generation.
//   AC1: eviction dirty arm drops the victim v1 / bridge / strings rows
//   AC2: eviction LRU arm same-generation invalidate
//   AC3: cache_define walker refuses an orphan v1 hit (production)
//   AC4: relower hit loop refuses; caller stays dirty / unstored until
//        a lower that did not embed the orphan
//   AC5: Soft keeps the observe-only consult contract
//   AC6: source cites; no new counter; no stray issue test file

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <format>

import std;
import aura.compiler.ir;
import aura.compiler.ir_cache_pure;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.dirty_propagation;

extern "C" void aura_hot_update_set_shape_storm_active(int);
extern "C" void aura_hot_update_reset_deopt_storm_state_for_test(void);

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::partial_relower_threshold_is_forced;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::should_partial_relower;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::get_strategy;
using aura::compiler::typed_audit::set_strategy;
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
"))")
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
"))")
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

    // ── #3986 AC4: type-change under Shape-only + empty persist ──
    {
        std::println("\n--- #3986 AC4: Shape-storm type-change soak ---");
        apply_production_audit_defaults();
        aura::compiler::dirty::reset_residual_castop_persist_for_test();
        aura_hot_update_reset_deopt_storm_state_for_test();
        aura_hot_update_set_shape_storm_active(1);
        CompilerService cs;
        CHECK(cs.eval(R"(
(set-code "
(define f (lambda () 1))
(define g (lambda () (f)))
"))")
                  .has_value(),
              "3986 AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3986 AC4: eval");
        if (!cs.get_define_v2("g"))
            (void)cs.eval("(compile:cache-define \"g\")");
        CHECK(cs.get_define_v2("g") != nullptr, "3986 AC4: g cached");
        cs.public_record_dependency("g", "f");
        const auto hash = cs.get_define_v2("g")->source_hash;
        const auto defuse0 = cs.get_define_v2("g")->version_stamp_.defuse_version;
        auto* m = metrics_of(cs);
        const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        auto mut = cs.eval("(mutate:set-body \"f\" \"(lambda () \\\"x\\\")\" \"#3986\")");
        CHECK(mut.has_value(), "3986 AC4: set-body type change");
        (void)cs.public_relower_dirty_defines_from_workspace();
        const auto* g1 = cs.get_define_v2("g");
        CHECK(g1 != nullptr, "3986 AC4: g after type-change");
        const int look = cs.lookup_define_v2("g", hash);
        CHECK(look == 1 || g1->dirty || g1->version_stamp_.defuse_version != defuse0 ||
                  !g1->content_stored_this_epoch ||
                  m->partial_forced_full_by_impact_total.load() > impact0,
              "3986 AC4: Shape-storm type-change did not silent-skip g");
        aura_hot_update_set_shape_storm_active(0);
        apply_dev_audit_defaults();
        aura::compiler::dirty::reset_residual_castop_persist_for_test();
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
"))")
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

    // Issue #4103: partial relower must not restamp while nested IR
    // after the dirty function is still the pre-mutate body.
    {
        std::println("\n--- #4103: nested IR after a partial peel is not a clean hit ---");
        struct Face {
            std::uint32_t strategy;
            std::uint32_t prod;
            std::size_t thr;
            bool thr_forced;
            Face()
                : strategy(
                      aura::compiler::typed_audit::g_typed_mutation_audit_counters.strategy.load(
                          std::memory_order_relaxed))
                , prod(aura::compiler::typed_audit::g_typed_mutation_audit_counters
                           .production_defaults_active.load(std::memory_order_relaxed))
                , thr(get_partial_relower_threshold())
                , thr_forced(partial_relower_threshold_is_forced()) {}
            ~Face() {
                aura::compiler::typed_audit::g_typed_mutation_audit_counters.strategy.store(
                    strategy, std::memory_order_relaxed);
                aura::compiler::typed_audit::g_typed_mutation_audit_counters
                    .production_defaults_active.store(prod, std::memory_order_relaxed);
                if (thr_forced)
                    set_partial_relower_threshold(thr);
                else
                    reset_partial_relower_threshold_for_test();
            }
        } face;
        (void)face;

        auto bundle_fp = [](const CompilerService::IRCacheEntry& e) {
            std::string s;
            for (const auto& fn : e.irs) {
                if (fn.name == "__top__")
                    continue;
                // cache_define renames __lambda__ to name#n. The full arm
                // does not. Compare instructions only.
                s += "{";
                for (const auto& b : fn.blocks) {
                    for (const auto& ins : b.instructions) {
                        s += std::format("{}:{}:{}:{}:{};", static_cast<unsigned>(ins.opcode),
                                         ins.operands[0], ins.operands[1], ins.operands[2],
                                         ins.operands[3]);
                    }
                    s += "|";
                }
                s += "}";
            }
            return s;
        };
        auto has_sentinel = [](const CompilerService::IRCacheEntry& e) {
            for (std::size_t fi = 2; fi < e.irs.size(); ++fi) {
                for (const auto& b : e.irs[fi].blocks) {
                    for (const auto& ins : b.instructions) {
                        if (ins.operands[3] == 0x4103u)
                            return true;
                    }
                }
            }
            return false;
        };
        const char* nested_src =
            R"src((set-code "(define (outer4103 x) (lambda (y) (+ y 1)))"))src";
        const char* plain_src = R"src((set-code "(define (plain4103 x) (+ x 1))"))src";

        auto cache_define = [](CompilerService& cs, const char* src, const char* name) {
            CHECK(cs.eval(src).has_value(), std::format("4103 set-code {}", name));
            CHECK(cs.eval("(eval-current)").has_value(), std::format("4103 eval {}", name));
            if (!cs.get_define_v2(name) || cs.get_define_v2(name)->irs.empty())
                (void)cs.eval(std::format("(compile:cache-define \"{}\")", name));
            const auto* e = cs.get_define_v2(name);
            return e != nullptr && !e->irs.empty();
        };

        // Production / Full: nested siblings fall through to a full store.
        {
            CompilerService fresh;
            CHECK(cache_define(fresh, nested_src, "outer4103"), "4103 fresh nested cache");
            const auto fresh_fp = fresh.get_define_v2("outer4103")
                                      ? bundle_fp(*fresh.get_define_v2("outer4103"))
                                      : std::string{};

            CompilerService cs;
            CHECK(cache_define(cs, nested_src, "outer4103"), "4103 nested cache");
            const auto planted = cs.plant_body_only_dirty_for_test("outer4103", true);
            CHECK(planted >= 3, std::format("4103 dual-shape irs.size={} >= 3", planted));
            const auto* before = cs.get_define_v2("outer4103");
            CHECK(before != nullptr && has_sentinel(*before), "4103 sentinel planted on nested IR");
            CHECK(!before->content_stored_this_epoch, "4103 content not stored while body-dirty");
            CHECK(cs.lookup_define_v2("outer4103", before->source_hash) == 1,
                  "4103 lookup stays 1 until a full store");
            set_strategy(AuditStrategy::Full);
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(1, std::memory_order_relaxed);
            // Keep the instruction peel from stealing the per-function arm.
            set_partial_relower_threshold(1);
            auto* m = metrics_of(cs);
            const auto impact0 =
                m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
            const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
            const auto per0 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
            // Issue #3760: an empty source_to_ir_map is an unknown cone.
            // Production mark_all_blocks_dirty then skips the per-fn arm.
            const bool map_empty = before->source_to_ir_map.empty();
            CHECK(cs.relower_define_from_workspace_for_test("outer4103"), "4103 nested relower");
            const auto* after = cs.get_define_v2("outer4103");
            CHECK(after != nullptr, "4103 entry after relower");
            CHECK(!has_sentinel(*after), "4103 full store replaced poisoned nested IR");
            CHECK(after->content_stored_this_epoch, "4103 content stored by full re-lower");
            CHECK(cs.lookup_define_v2("outer4103", after->source_hash) == 0,
                  "4103 lookup 0 only after the nested function was replaced");
            CHECK(bundle_fp(*after) == fresh_fp, "4103 nested IR matches a full lower");
            CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) > impact0,
                  "4103 reuses partial_forced_full_by_impact_total");
            CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) > full0,
                  "4103 fell through to full re-lower");
            const auto per1 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
            CHECK(map_empty ? per1 == per0 : per1 > per0,
                  "4103 empty source map force-fulls before per-fn");
        }

        // __top__ + body (irs.size()==2) may still peel and clean-hit.
        {
            CompilerService cs;
            CHECK(cache_define(cs, plain_src, "plain4103"), "4103 plain cache");
            const auto planted = cs.plant_body_only_dirty_for_test("plain4103", false);
            CHECK(planted == 2, std::format("4103 plain irs.size={} == 2", planted));
            const auto* before = cs.get_define_v2("plain4103");
            CHECK(before != nullptr, "4103 plain entry");
            CHECK(!before->irs.empty() && before->irs[0].name == "__top__",
                  "4103 plain bundle still starts with __top__");
            CHECK(before->block_dirty_view().size() >= 2, "4103 plain dual mask");
            {
                bool top_clean = true;
                for (auto b : before->block_dirty_view()[0])
                    if (b)
                        top_clean = false;
                bool body_dirty = false;
                for (auto b : before->block_dirty_view()[1])
                    if (b)
                        body_dirty = true;
                CHECK(top_clean, "4103 __top__ at index 0 stays clean");
                CHECK(body_dirty, "4103 body at index 1 is dirty");
            }
            CHECK(cs.lookup_define_v2("plain4103", before->source_hash) == 1,
                  "4103 plain lookup 1 while body-dirty");
            set_strategy(AuditStrategy::Full);
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(1, std::memory_order_relaxed);
            set_partial_relower_threshold(1);
            auto* m = metrics_of(cs);
            const auto impact0 =
                m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
            const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
            const auto per0 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
            CHECK(cs.relower_define_from_workspace_for_test("plain4103"), "4103 plain relower");
            const auto* after = cs.get_define_v2("plain4103");
            CHECK(after != nullptr && after->content_stored_this_epoch,
                  "4103 plain content stored");
            CHECK(cs.lookup_define_v2("plain4103", after->source_hash) == 0,
                  "4103 plain peel clean-hits");
            CHECK(m->relower_per_function_called_count.load(std::memory_order_relaxed) > per0,
                  "4103 plain took the per-function peel");
            CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) == full0,
                  "4103 plain peel did not fall through to full");
            CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) == impact0,
                  "4103 plain peel does not force-full");
        }

        // Soft / Off keeps the single-function return, nested sentinel included.
        // Cache while production is off. A service built with the atomic
        // still 1 stores a source-only shell and irs stays empty.
        {
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(0, std::memory_order_relaxed);
            set_strategy(AuditStrategy::Sampled);
            CompilerService cs;
            CHECK(cache_define(cs, nested_src, "outer4103"), "4103 soft nested cache");
            const auto planted = cs.plant_body_only_dirty_for_test("outer4103", true);
            CHECK(planted >= 3, std::format("4103 soft irs.size={} >= 3", planted));
            set_partial_relower_threshold(1);
            auto* m = metrics_of(cs);
            const auto impact0 =
                m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
            const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
            CHECK(cs.relower_define_from_workspace_for_test("outer4103"), "4103 soft relower");
            const auto* after = cs.get_define_v2("outer4103");
            CHECK(after != nullptr && has_sentinel(*after),
                  "4103 Soft peel leaves nested IR on the pre-mutate body");
            CHECK(after->content_stored_this_epoch, "4103 Soft peel stores the peeled function");
            CHECK(cs.lookup_define_v2("outer4103", after->source_hash) == 0,
                  "4103 Soft single-function peel clean-hits");
            CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) == impact0,
                  "4103 Soft peel does not force-full");
            CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) == full0,
                  "4103 Soft peel does not full-relower");
        }

        auto any_dirty = [](const std::vector<std::uint8_t>& mask) {
            for (auto b : mask)
                if (b)
                    return true;
            return false;
        };
        auto poisoned_after_body = [](const CompilerService::IRCacheEntry& e) {
            const std::size_t body = (e.irs.size() >= 2 && e.irs[0].name == "__top__") ? 1 : 0;
            for (std::size_t fi = body + 1; fi < e.irs.size(); ++fi) {
                for (const auto& b : e.irs[fi].blocks) {
                    for (const auto& ins : b.instructions) {
                        if (ins.operands[3] == 0x4103u)
                            return true;
                    }
                }
            }
            return false;
        };
        auto has_const_i64 = [](const aura::ir::IRFunction& fn, std::uint32_t imm) {
            for (const auto& b : fn.blocks) {
                for (const auto& ins : b.instructions) {
                    if (ins.opcode == aura::ir::IROpcode::ConstI64 && ins.operands[1] == imm)
                        return true;
                }
            }
            return false;
        };

        // Issue #4311: production store shape [body, nested] (no __top__).
        // Body-dirty hits index 0; #4103 size > dirty_idx+1 force-fulls
        // before any restamp. Nested poison must not survive.
        {
            CompilerService fresh;
            CHECK(cache_define(fresh, nested_src, "outer4103"), "4311 fresh nested cache");
            const auto* fresh_e = fresh.get_define_v2("outer4103");
            CHECK(fresh_e != nullptr && fresh_e->irs.size() == 2, "4311 fresh irs.size()==2");
            CHECK(fresh_e->irs[0].name != "__top__", "4311 fresh cache dropped __top__");
            const auto fresh_fp = bundle_fp(*fresh_e);

            CompilerService cs;
            CHECK(cache_define(cs, nested_src, "outer4103"), "4311 nested cache");
            const auto planted = cs.plant_body_only_dirty_for_test("outer4103", true, false);
            CHECK(planted == 2, std::format("4311 dropped-top irs.size={} == 2", planted));
            const auto* before = cs.get_define_v2("outer4103");
            CHECK(before != nullptr && before->irs[0].name != "__top__",
                  "4311 plant does not prepend __top__");
            CHECK(before->block_dirty_view().size() >= 2, "4311 dual mask");
            CHECK(any_dirty(before->block_dirty_view()[0]), "4311 dirties the real body at 0");
            CHECK(!any_dirty(before->block_dirty_view()[1]),
                  "4311 leaves the nested function clean");
            CHECK(poisoned_after_body(*before), "4311 sentinel planted on nested IR");
            CHECK(!before->content_stored_this_epoch, "4311 content not stored while body-dirty");
            CHECK(cs.lookup_define_v2("outer4103", before->source_hash) == 1,
                  "4311 lookup stays 1 until a full store");
            set_strategy(AuditStrategy::Full);
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(1, std::memory_order_relaxed);
            set_partial_relower_threshold(1);
            auto* m = metrics_of(cs);
            const auto impact0 =
                m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
            const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
            const auto per0 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
            const bool map_empty = before->source_to_ir_map.empty();
            CHECK(cs.relower_define_from_workspace_for_test("outer4103"), "4311 nested relower");
            const auto* after = cs.get_define_v2("outer4103");
            CHECK(after != nullptr, "4311 entry after relower");
            CHECK(!poisoned_after_body(*after), "4311 full store replaced poisoned nested IR");
            CHECK(after->content_stored_this_epoch, "4311 content stored by full re-lower");
            CHECK(cs.lookup_define_v2("outer4103", after->source_hash) == 0,
                  "4311 lookup 0 only after the body was replaced");
            CHECK(bundle_fp(*after) == fresh_fp, "4311 IR matches a full lower");
            CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) > impact0,
                  "4311 reuses partial_forced_full_by_impact_total");
            CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) > full0,
                  "4311 fell through to full re-lower");
            const auto per1 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
            CHECK(map_empty ? per1 == per0 : per1 > per0,
                  "4311 empty source map force-fulls before per-fn");
        }

        // Size == 1 dropped-__top__ still peels (no nested sibling). The
        // workspace impact_ub==0 full fallback is unchanged; this direct
        // relower must not grow the #4103 sibling gate onto size 1.
        {
            CompilerService cs;
            CHECK(cache_define(cs, plain_src, "plain4103"), "4311 size1 cache");
            const auto planted = cs.plant_body_only_dirty_for_test("plain4103", false, false);
            CHECK(planted == 1, std::format("4311 size1 irs.size={} == 1", planted));
            const auto* before = cs.get_define_v2("plain4103");
            CHECK(before != nullptr && before->irs[0].name != "__top__",
                  "4311 size1 has no __top__");
            CHECK(!before->block_dirty_view().empty() && any_dirty(before->block_dirty_view()[0]),
                  "4311 size1 dirties index 0");
            set_strategy(AuditStrategy::Full);
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(1, std::memory_order_relaxed);
            set_partial_relower_threshold(1);
            auto* m = metrics_of(cs);
            const auto impact0 =
                m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
            const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
            const auto per0 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
            CHECK(cs.relower_define_from_workspace_for_test("plain4103"), "4311 size1 relower");
            const auto* after = cs.get_define_v2("plain4103");
            CHECK(after != nullptr && after->content_stored_this_epoch,
                  "4311 size1 content stored");
            CHECK(cs.lookup_define_v2("plain4103", after->source_hash) == 0,
                  "4311 size1 peel clean-hits");
            CHECK(m->relower_per_function_called_count.load(std::memory_order_relaxed) > per0,
                  "4311 size1 took the per-function peel");
            CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) == full0,
                  "4311 size1 did not fall through to full");
            CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) == impact0,
                  "4311 size1 does not force-full");
        }

        // Soft / Off on the production shape: zero-extra peel stays.
        // Dirties index 0 and leaves the nested sentinel in place.
        // Cache while production is off (same shell as the #4103 soft block).
        {
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(0, std::memory_order_relaxed);
            set_strategy(AuditStrategy::Sampled);
            CompilerService cs;
            CHECK(cache_define(cs, nested_src, "outer4103"), "4311 soft nested cache");
            const auto planted = cs.plant_body_only_dirty_for_test("outer4103", true, false);
            CHECK(planted == 2, std::format("4311 soft irs.size={} == 2", planted));
            set_partial_relower_threshold(1);
            auto* m = metrics_of(cs);
            const auto impact0 =
                m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
            const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
            CHECK(cs.relower_define_from_workspace_for_test("outer4103"), "4311 soft relower");
            const auto* after = cs.get_define_v2("outer4103");
            CHECK(after != nullptr && poisoned_after_body(*after),
                  "4311 Soft peel leaves nested IR on the pre-mutate body");
            CHECK(after->content_stored_this_epoch, "4311 Soft peel stores the peeled function");
            CHECK(cs.lookup_define_v2("outer4103", after->source_hash) == 0,
                  "4311 Soft single-function peel clean-hits");
            CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) == impact0,
                  "4311 Soft peel does not force-full");
            CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) == full0,
                  "4311 Soft peel does not full-relower");
        }

        // Production body-expression set-body. Nested IR is one straight-line
        // function with >= default threshold instructions and < threshold
        // blocks. lookup must not clean-hit while irs[0] still lacks the
        // new body's constant. let (not cons) keeps the return type a
        // lambda so production solve_delta stays SOLVED; a rejected
        // mutate abort-clears irs before the sweep.
        {
            reset_partial_relower_threshold_for_test();
            const char* fat_src =
                R"src((set-code "(define (fat4311 x) (lambda (y) (+ (+ (+ (+ (+ (+ (+ (+ y 1) 2) 3) 4) 5) 6) 7) 8)))"))src";
            // Cache on the dev face. Production=1 at construct stores a
            // source-only shell (irs empty) and the nested index aborts.
            CompilerService cs;
            CHECK(cache_define(cs, fat_src, "fat4311"), "4311 fat cache");
            set_strategy(AuditStrategy::Full);
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(1, std::memory_order_relaxed);
            const auto thr_now = get_partial_relower_threshold();
            const auto* pre = cs.get_define_v2("fat4311");
            CHECK(pre != nullptr && pre->irs.size() == 2, "4311 fat shape [body, nested]");
            if (pre != nullptr && pre->irs.size() >= 2) {
                CHECK(pre->irs[0].name != "__top__", "4311 fat dropped __top__");
                std::size_t nested_instrs = 0;
                for (const auto& b : pre->irs[1].blocks)
                    nested_instrs += b.instructions.size();
                const auto nested_blocks = pre->irs[1].blocks.size();
                CHECK(nested_instrs >= thr_now,
                      std::format("4311 nested instrs {} >= threshold {}", nested_instrs, thr_now));
                CHECK(nested_blocks > 0 && nested_blocks < thr_now,
                      std::format("4311 nested blocks {} < threshold {}", nested_blocks, thr_now));
                CHECK(!has_const_i64(pre->irs[0], 41), "4311 pre-mutate body has no 41");
                auto* m = metrics_of(cs);
                const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
                const auto ran0 = m->cascade_relower_ran_total.load(std::memory_order_relaxed);
                auto setb = cs.eval(
                    R"ev((mutate:set-body "fat4311" "(let ((z (+ x 41))) (lambda (y) (+ (+ (+ (+ (+ (+ (+ (+ y 1) 2) 3) 4) 5) 6) 7) z)))" "#4311"))ev");
                CHECK(setb && aura::compiler::types::is_bool(*setb) &&
                          aura::compiler::types::as_bool(*setb),
                      "4311 body-expression set-body");
                const auto* after = cs.get_define_v2("fat4311");
                CHECK(after != nullptr && !after->irs.empty(), "4311 entry after set-body has IR");
                if (after != nullptr && !after->irs.empty()) {
                    CHECK(after->irs[0].name != "__top__",
                          "4311 post-mutate cache still dropped __top__");
                    const bool body_rewritten = has_const_i64(after->irs[0], 41);
                    const int look = cs.lookup_define_v2("fat4311", after->source_hash);
                    CHECK(body_rewritten, "4311 sweep rewrote irs[0] to the new body");
                    CHECK(look != 0 || body_rewritten,
                          "4311 lookup must not clean-hit a pre-mutate body");
                }
                CHECK(m->cascade_relower_ran_total.load(std::memory_order_relaxed) > ran0,
                      "4311 cascade relower ran");
                CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) > full0,
                      "4311 sweep took full re-lower");
            }
        }

        // Source cite: new gate, plus unknown-callee and abort-stale still force full.
        {
            const auto svc = read_file("src/compiler/service.ixx");
            const auto at = svc.find("Issue #4103:");
            CHECK(at != std::string::npos, "4103 cite present");
            const auto win = svc.substr(at, 1400);
            CHECK(win.find("it->second.irs.size() > dirty_func_idx + 1") != std::string::npos,
                  "4103 cite: sibling size check");
            CHECK(win.find("partial_forced_full_by_impact_total") != std::string::npos,
                  "4103 cite: reuse partial_forced_full_by_impact_total");
            CHECK(win.find("production_defaults_active()") != std::string::npos,
                  "4103 cite: production_defaults_active");
            CHECK(win.find("AuditStrategy::Full") != std::string::npos, "4103 cite: Full");
            CHECK(win.find("content_stored_this_epoch = false") != std::string::npos,
                  "4103 cite: do not store content on the sibling shape");
            CHECK(win.find("restamp_cache_entry_live_") == std::string::npos,
                  "4103 cite: sibling arm does not restamp");
            const auto unk = svc.find("kUnknownCalleeConeBlocks");
            CHECK(unk != std::string::npos, "4103 unknown-callee gate still present");
            const auto unk_win = svc.substr(unk > 400 ? unk - 400 : 0, 900);
            CHECK(unk_win.find("partial_forced_full_by_impact_total") != std::string::npos,
                  "4103 unknown-callee still forces full");
            CHECK(unk_win.find("allow_partial = false") != std::string::npos,
                  "4103 unknown-callee clears allow_partial");
            const auto abort_at = svc.find("Issue #3324: abort-restore must not instr-peel");
            CHECK(abort_at != std::string::npos, "4103 abort-stale gate still present");
            const auto abort_win = svc.substr(abort_at, 700);
            CHECK(abort_win.find("content_stored_this_epoch = false") != std::string::npos,
                  "4103 abort-stale still refuses a content store");
            CHECK(abort_win.find("mark_all_blocks_dirty()") != std::string::npos,
                  "4103 abort-stale still marks the entry dirty");
            CHECK(read_file("tests/compiler/test_issue_4103.cpp").empty(),
                  "4103 no test_issue_4103.cpp");
            CHECK(read_file("docs/design/4103-nested-relower.md").empty(), "4103 no design note");
            const auto at4311 = svc.find("Issue #4311:");
            CHECK(at4311 != std::string::npos, "4311 cite present");
            const auto win4311 = svc.substr(at4311, 700);
            CHECK(win4311.find("irs[0].name == \"__top__\"") != std::string::npos,
                  "4311 cite: body index checks __top__");
            CHECK(win4311.find("return 0") != std::string::npos,
                  "4311 cite: dropped __top__ stays index 0");
            const auto v1at = svc.find("Issue #4311: write [func_idx]");
            CHECK(v1at != std::string::npos, "4311 v1 mirror cite");
            const auto v1win = svc.substr(v1at, 1200);
            CHECK(v1win.find("func_idx - 1") != std::string::npos,
                  "4311 cite: __top__ prefix keeps func_idx-1");
            CHECK(v1win.find("v1_it->second[func_idx]") != std::string::npos,
                  "4311 cite: dropped-top v1 uses func_idx");
            CHECK(read_file("tests/compiler/test_issue_4311.cpp").empty(),
                  "4311 no test_issue_4311.cpp");
            CHECK(read_file("docs/design/4311-dropped-top-body.md").empty(), "4311 no design note");
        }
    }

    // Issue #4436: production full relower must not publish a caller that
    // copied a still-dirty callee. The direct full-arm call refuses. The
    // sweep retries once and stores the post-mutate constant.
    {
        std::println("\n--- #4436: full relower refuses a dirty callee embed ---");
        apply_production_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval(R"(
(set-code "
(define f (lambda () 1))
(define g (lambda () (f)))
"))")
                  .has_value(),
              "4436: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4436: eval");
        if (!cs.get_define_v2("f"))
            (void)cs.eval("(compile:cache-define \"f\")");
        if (!cs.get_define_v2("g"))
            (void)cs.eval("(compile:cache-define \"g\")");
        CHECK(cs.get_define_v2("f") != nullptr, "4436: f cached");
        CHECK(cs.get_define_v2("g") != nullptr, "4436: g cached");
        cs.public_record_dependency("g", "f");
        auto mut = cs.eval("(mutate:set-body \"f\" \"(lambda () 2)\" \"#4436\")");
        CHECK(mut && aura::compiler::types::is_bool(*mut) && aura::compiler::types::as_bool(*mut),
              "4436: set-body");
        cs.public_mark_define_dirty("f");
        // #4228 leaves f as a value cell, so g lowers to TopCellLoad and
        // never copies ir_cache_[f]. The hole is that copy.
        cs.clear_ir_value_cells_for_test();
        const auto* f_dirty = cs.get_define_v2("f");
        const auto* g_dirty = cs.get_define_v2("g");
        CHECK(f_dirty != nullptr && (f_dirty->dirty || f_dirty->dirty_block_count() > 0),
              "4436: f stays body-dirty before the direct full relower");
        CHECK(g_dirty != nullptr, "4436: g still cached");
        const bool direct = cs.relower_current_define_full_for_test("g");
        CHECK(!direct, "4436: direct full relower refuses the dirty callee");
        const auto* g_mid = cs.get_define_v2("g");
        CHECK(g_mid != nullptr, "4436: g still cached after refuse");
        if (g_mid != nullptr) {
            const int look_mid = cs.lookup_define_v2("g", g_mid->source_hash);
            CHECK(look_mid != 0, "4436: lookup stays needs-relower after refuse");
            CHECK(g_mid->dirty || !g_mid->content_stored_this_epoch,
                  "4436: refuse does not store g");
        }
        (void)cs.public_relower_dirty_defines_from_workspace();
        const auto* g_after = cs.get_define_v2("g");
        CHECK(g_after != nullptr, "4436: g after sweep");
        if (g_after != nullptr) {
            const int look_after = cs.lookup_define_v2("g", g_after->source_hash);
            auto bundle_has = [](const CompilerService::IRCacheEntry& e, std::uint32_t imm) {
                for (const auto& fn : e.irs) {
                    for (const auto& b : fn.blocks) {
                        for (const auto& ins : b.instructions) {
                            if (ins.opcode == aura::ir::IROpcode::ConstI64 &&
                                ins.operands[1] == imm)
                                return true;
                        }
                    }
                }
                return false;
            };
            CHECK(look_after == 0, "4436: sweep stores g as a clean hit");
            CHECK(bundle_has(*g_after, 2), "4436: stored g embeds the post-mutate callee");
        }
        const auto svc = read_file("src/compiler/service.ixx");
        const auto at = svc.find("Issue #4436");
        CHECK(at != std::string::npos, "4436 cite present");
        if (at != std::string::npos) {
            // Window widened by #4528: the orphan-v1 refuse arm extends
            // the same block before the latch checks.
            const auto win = svc.substr(at, 2200);
            CHECK(win.find("production_defaults_active()") != std::string::npos,
                  "4436 cite: production gate");
            CHECK(win.find("content_stored_this_epoch") != std::string::npos,
                  "4436 cite: content latch");
            CHECK(win.find("abort_map_invalid") != std::string::npos, "4436 cite: abort latch");
        }
        CHECK(svc.find("relower_current_define_full_for_test") != std::string::npos,
              "4436 cite: full-arm test hook");
        apply_dev_audit_defaults();
    }

    // Issue #4513: cache_define republishes pre-abort ir_cache_ callee
    // bodies after the v2 freshness refuse. Production / Full must drop
    // the walked names' v1 / bridge / strings rows on abort, refuse the
    // v1 callee copy in cache_define while the callee's v2 row is
    // untrusted (latched or merely dirty), and keep the relower full arm
    // from storing a body that still references an untrusted callee.
    {
        std::println("\n--- #4513: abort drops v1 callee bytes + fail-closed cache_define ---");
        apply_production_audit_defaults();
        CompilerService cs;
        // Single-define set-codes: multi-define set-code is broken on the
        // current tree (pre-existing — see the #3484 AC5 soak failure).
        CHECK(cs.eval("(set-code \"(define b4513 (lambda () 1))\")").has_value(),
              "4513: set-code b");
        CHECK(cs.eval("(eval-current)").has_value(), "4513: eval b");
        CHECK(cs.get_define_v2("b4513") != nullptr, "4513: b cached");
        CHECK(cs.has_cached_function("b4513"), "4513: b v1 present pre-abort");
        CHECK(cs.eval("(set-code \"(define a4513 (lambda () (b4513)))\")").has_value(),
              "4513: set-code a");
        CHECK(cs.eval("(eval-current)").has_value(), "4513: eval a");
        // a's cache state right after its define is tree-flow dependent
        // (a dirty-callee consult may fail it closed to eval_flat); the
        // safety contract below holds in every face.
        const bool a_v1_pre = cs.has_cached_function("a4513");

        // Repro steps 2-3: mutate b inside a boundary, then abort the
        // boundary — dual-topology restore + force_ir_cache_dirty_after_abort.
        auto& ev4513 = cs.evaluator();
        ev4513.enter_mutation_boundary();
        auto mut4513 = cs.eval("(mutate:set-body \"b4513\" \"(lambda () 2)\" \"#4513\")");
        CHECK(mut4513.has_value(), "4513: set-body inside boundary");
        ev4513.exit_mutation_boundary(false);

        const auto* b_abort = cs.get_define_v2("b4513");
        CHECK(b_abort != nullptr && b_abort->abort_map_invalid, "4513: b v2 abort-latched");
        CHECK(b_abort != nullptr && !b_abort->content_stored_this_epoch,
              "4513: b v2 content unstored");
        // AC1: the walked names' v1 bundles are gone — the aborted bytes
        // cannot be copied into the next define.
        CHECK(!cs.has_cached_function("b4513"), "4513 AC1: b v1 dropped after abort");
        if (a_v1_pre)
            CHECK(!cs.has_cached_function("a4513"), "4513 AC1: a v1 dropped after abort");

        // Repro steps 4-7: the next eval runs the sweep + populate. The
        // aborted bytes (ConstI64 2) must never be republished: not in b's
        // row, not in a's row, and calling a must run the restored body.
        cs.clear_ir_value_cells_for_test(); // #4228: reach the copy face.
        CHECK(cs.eval("(eval-current)").has_value(), "4513: post-abort eval-current");
        auto bundle_has4513 = [](const CompilerService::IRCacheEntry& e, std::uint32_t imm) {
            for (const auto& fn : e.irs) {
                for (const auto& b : fn.blocks) {
                    for (const auto& ins : b.instructions) {
                        if (ins.opcode == aura::ir::IROpcode::ConstI64 && ins.operands[1] == imm)
                            return true;
                    }
                }
            }
            return false;
        };
        auto call4513 = cs.eval("(a4513)");
        CHECK(call4513 && aura::compiler::types::is_int(*call4513) &&
                  aura::compiler::types::as_int(*call4513) == 1,
              "4513 AC4: a runs the restored body");
        // Convergence: a second eval-current lets the sweep + populate
        // re-store the walked names from the restored AST. Whatever rows
        // exist afterwards must hold the restored body and only the
        // restored body.
        CHECK(cs.eval("(eval-current)").has_value(), "4513: convergence eval-current");
        const auto* b_after = cs.get_define_v2("b4513");
        CHECK(b_after != nullptr, "4513 AC2: b v2 row survives");
        if (b_after != nullptr) {
            CHECK(!bundle_has4513(*b_after, 2), "4513 AC2: aborted bytes are gone");
            if (!b_after->dirty && b_after->content_stored_this_epoch &&
                !b_after->abort_map_invalid) {
                CHECK(bundle_has4513(*b_after, 1), "4513 AC2: clean b holds the restored body");
            }
        }
        const auto* a_after = cs.get_define_v2("a4513");
        if (a_after != nullptr) {
            // The #4228 value-cell face lowers the callee reference as a
            // live TopCellLoad (no bundle embed), so the only universal
            // embed contract is the negative one: never the aborted body.
            CHECK(!bundle_has4513(*a_after, 2), "4513 AC3: a never embeds the aborted callee");
        }
        auto call4513b = cs.eval("(a4513)");
        CHECK(call4513b && aura::compiler::types::is_int(*call4513b) &&
                  aura::compiler::types::as_int(*call4513b) == 1,
              "4513 AC4: a still runs the restored body after convergence");
        apply_dev_audit_defaults();
    }

    // Issue #4513 AC5: cache_define refuses a v1 callee hit whose v2 row
    // is dirty even with the abort latch clear (impact-refuse face — v1
    // holds the previous body), and drops the caller's own stale bundle
    // so populate cannot store_define_v2 it back. Fully direct-define
    // flow: the callee name is underscore-prefixed so the workspace sweep
    // and populate structurally skip it and nothing re-stores it mid-flow.
    {
        std::println("\n--- #4513 AC5: cache_define consult refuses a dirty v1 callee ---");
        apply_production_audit_defaults();
        CompilerService cs;
        // Workspace anchor (first direct define installs its own flat).
        CHECK(cs.eval("(define z4513 (lambda () 0))").has_value(), "4513 AC5: anchor define z");
        // The callee: v1 bundle + a clean content-stored v2 row (#3117
        // AC3 planting pattern). Underscore name → sweep + populate skip.
        CHECK(cs.eval("(define _c4513 (lambda () 1))").has_value(), "4513 AC5: direct define c");
        CHECK(cs.has_cached_function("_c4513"), "4513 AC5: c v1 present");
        {
            aura::ir::IRFunction top4513;
            top4513.id = 0;
            top4513.name = "__top__";
            top4513.blocks.push_back({0, {}, {}});
            aura::ir::IRFunction body4513;
            body4513.id = 1;
            body4513.name = "_c4513#0";
            body4513.blocks.push_back({0, {}, {}});
            std::vector<aura::ir::IRFunction> irs4513;
            irs4513.push_back(std::move(top4513));
            irs4513.push_back(std::move(body4513));
            cs.store_define_v2("_c4513", "(define _c4513 (lambda () 1))", std::move(irs4513), {},
                               {}, cs.public_mutation_epoch());
        }
        const auto* c_row = cs.get_define_v2("_c4513");
        CHECK(c_row != nullptr && !c_row->dirty && c_row->content_stored_this_epoch &&
                  !c_row->abort_map_invalid,
              "4513 AC5: c row clean with clear latch");
        // First d: the consult passes (c clean) — the embed is a fresh copy.
        CHECK(cs.eval("(define d4513 (lambda () 0 (_c4513)))").has_value(),
              "4513 AC5: direct define d");
        CHECK(cs.has_cached_function("d4513"), "4513 AC5: d v1 present");
        cs.clear_ir_value_cells_for_test();
        // Dirty c's v2 row WITHOUT abort: v1[c] now holds the previous
        // body while the row is untrusted. Nothing can re-store it (the
        // sweep skips — not in the [z] workspace — and populate skips the
        // underscore name), so the consult is the only face that runs.
        cs.public_mark_define_dirty("_c4513");
        c_row = cs.get_define_v2("_c4513");
        CHECK(c_row != nullptr && c_row->dirty && !c_row->abort_map_invalid,
              "4513 AC5: c dirty with clear latch");
        // Redefine d: the walker must refuse the v1[c] hit and fail
        // closed to eval_flat, dropping d's own stale embed.
        CHECK(cs.eval("(define d4513 (lambda () 7 (_c4513)))").has_value(),
              "4513 AC5: redefine d drives cache_define");
        CHECK(!cs.has_cached_function("d4513"), "4513 AC5: d v1 refused — stale embed dropped");
        c_row = cs.get_define_v2("_c4513");
        CHECK(c_row != nullptr && c_row->dirty,
              "4513 AC5: c stays dirty (refuse does not restamp)");
        apply_dev_audit_defaults();
    }

    // Issue #4513 AC6: Soft / Off keeps the observe-only abort contract —
    // the walked names' v1 bundles survive the abort (no drop), and the
    // cache_define consult is production-gated off.
    {
        std::println("\n--- #4513 AC6: Soft keeps the v1 observe-only contract ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define e4513 (lambda () 1))\")").has_value(),
              "4513 AC6: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4513 AC6: eval");
        if (!cs.get_define_v2("e4513"))
            (void)cs.eval("(compile:cache-define \"e4513\")");
        CHECK(cs.has_cached_function("e4513"), "4513 AC6: v1 present pre-abort");
        auto& ev_s = cs.evaluator();
        ev_s.enter_mutation_boundary();
        (void)cs.eval("(mutate:set-body \"e4513\" \"(lambda () 2)\" \"#4513\")");
        ev_s.exit_mutation_boundary(false);
        CHECK(cs.get_define_v2("e4513") != nullptr && cs.get_define_v2("e4513")->abort_map_invalid,
              "4513 AC6: v2 latch still set under Soft");
        CHECK(cs.has_cached_function("e4513"), "4513 AC6: Soft keeps v1 (observe-only, no drop)");
        apply_dev_audit_defaults();
    }

    // Issue #4513 AC7: source cites + no new counters + no stray issue
    // test file.
    {
        const auto svc4513 = read_file("src/compiler/service.ixx");
        CHECK(read_file("tests/compiler/test_issue_4513.cpp").empty(),
              "4513 AC7: no tests/issues/test_issue_4513.cpp");
        CHECK(svc4513.find("g_4513_") == std::string::npos, "4513 AC7: no new file-scope counter");
        CHECK(svc4513.find("4513_total") == std::string::npos, "4513 AC7: no new metrics counter");
        const auto c1 = svc4513.find("Issue #4513: cache_define's LambdaBodyWalker");
        CHECK(c1 != std::string::npos, "4513 AC7: abort-drop cite present");
        if (c1 != std::string::npos) {
            const auto win = svc4513.substr(c1, 3400);
            CHECK(win.find("production_defaults_active()") != std::string::npos,
                  "4513 AC7: drop is production-gated");
            CHECK(win.find("ir_cache_bridge_") != std::string::npos, "4513 AC7: bridge drop cite");
            CHECK(win.find("ir_cache_strings_") != std::string::npos,
                  "4513 AC7: strings drop cite");
        }
        const auto c2 = svc4513.find("Issue #4513: v1 presence alone is not");
        CHECK(c2 != std::string::npos, "4513 AC7: consult cite present");
        if (c2 != std::string::npos) {
            const auto win = svc4513.substr(c2, 1800);
            CHECK(win.find("content_stored_this_epoch") != std::string::npos,
                  "4513 AC7: consult checks content latch");
            CHECK(win.find("abort_map_invalid") != std::string::npos,
                  "4513 AC7: consult checks abort latch");
        }
        const auto c3 = svc4513.find("Issue #4513: the abort walk drops the walked names");
        CHECK(c3 != std::string::npos, "4513 AC7: relower scan cite present");
        if (c3 != std::string::npos) {
            const auto win = svc4513.substr(c3, 3600);
            CHECK(win.find("UntrustedCalleeScan4513") != std::string::npos,
                  "4513 AC7: body scan present");
            CHECK(win.find("embed_safe") != std::string::npos, "4513 AC7: cone exemption kept");
        }
    }

    // Issue #4528 AC4: the relower full arm's hit loop treats a v1 cache
    // hit whose v2 row is absent as untrusted (same predicate family as a
    // dirty row): the store is refused, the caller stays dirty with
    // content_stored_this_epoch == false, and after the callee's v2 trust
    // row is restored the next full lower stores again (no deadlock).
    // Cache setup runs on the dev face (workspace populate is
    // set-code-driven and observe-only there); the consult under test
    // reads the live face at every site, so the orphan phase arms
    // production.
    {
        std::println("\n--- #4528 AC4: relower hit loop refuses a v1 hit with no v2 row ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f4528 (lambda () 1))\")").has_value(),
              "4528 AC4: set-code f");
        CHECK(cs.eval("(eval-current)").has_value(), "4528 AC4: eval f");
        CHECK(cs.get_define_v2("f4528") != nullptr, "4528 AC4: f v2 cached");
        CHECK(cs.has_cached_function("f4528"), "4528 AC4: f v1 present");
        CHECK(cs.eval("(set-code \"(define g4528 (lambda () (f4528)))\")").has_value(),
              "4528 AC4: set-code g");
        CHECK(cs.eval("(eval-current)").has_value(), "4528 AC4: eval g");
        CHECK(cs.get_define_v2("g4528") != nullptr, "4528 AC4: g v2 cached");
        cs.public_record_dependency("g4528", "f4528");
        // Orphan phase: production arms the freshness consults.
        apply_production_audit_defaults();
        // Orphan the v1 row: erase ONLY f's v2 row (what pre-#4528
        // eviction did to its victim). v1 keeps the lowered body.
        CHECK(cs.erase_ir_cache_v2_row_for_test("f4528"), "4528 AC4: f v2 row erased");
        CHECK(cs.get_define_v2("f4528") == nullptr, "4528 AC4: f v2 gone");
        CHECK(cs.has_cached_function("f4528"), "4528 AC4: orphan v1 survives");
        cs.public_mark_define_dirty("g4528");
        // #4228 leaves the callee as a value cell; drop the cells to reach
        // the v1 copy face.
        cs.clear_ir_value_cells_for_test();
        const bool direct4528 = cs.relower_current_define_full_for_test("g4528");
        CHECK(!direct4528, "4528 AC4: full relower refuses the orphan v1 embed");
        const auto* g4528 = cs.get_define_v2("g4528");
        CHECK(g4528 != nullptr, "4528 AC4: g row survives the refuse");
        if (g4528 != nullptr) {
            CHECK(cs.lookup_define_v2("g4528", g4528->source_hash) != 0,
                  "4528 AC4: lookup stays needs-relower");
            CHECK(g4528->dirty && !g4528->content_stored_this_epoch,
                  "4528 AC4: caller dirty with content_stored_this_epoch == false");
        }
        // Convergence: restore f's v2 trust row (#3117 AC3 planting
        // pattern) — the next full lower trusts the pair and stores.
        {
            aura::ir::IRFunction top4528;
            top4528.id = 0;
            top4528.name = "__top__";
            top4528.blocks.push_back({0, {}, {}});
            std::vector<aura::ir::IRFunction> irs4528;
            irs4528.push_back(std::move(top4528));
            cs.store_define_v2("f4528", "(define f4528 (lambda () 1))", std::move(irs4528), {}, {},
                               cs.public_mutation_epoch());
        }
        {
            const auto* f4528 = cs.get_define_v2("f4528");
            CHECK(f4528 != nullptr && !f4528->dirty && f4528->content_stored_this_epoch,
                  "4528 AC4: f v2 trust row restored");
        }
        cs.clear_ir_value_cells_for_test();
        CHECK(cs.relower_current_define_full_for_test("g4528"),
              "4528 AC4: g full relower stores after f is trusted again");
        const auto* g_after4528 = cs.get_define_v2("g4528");
        CHECK(g_after4528 != nullptr && !g_after4528->dirty &&
                  g_after4528->content_stored_this_epoch,
              "4528 AC4: g stored as a clean generation");
        if (g_after4528 != nullptr) {
            CHECK(cs.lookup_define_v2("g4528", g_after4528->source_hash) == 0,
                  "4528 AC4: lookup hits after convergence");
            auto bundle_has4528 = [](const CompilerService::IRCacheEntry& e, std::uint32_t imm) {
                for (const auto& fn : e.irs) {
                    for (const auto& b : fn.blocks) {
                        for (const auto& ins : b.instructions) {
                            if (ins.opcode == aura::ir::IROpcode::ConstI64 &&
                                ins.operands[1] == imm)
                                return true;
                        }
                    }
                }
                return false;
            };
            CHECK(bundle_has4528(*g_after4528, 1), "4528 AC4: stored g embeds the restored f body");
        }
        apply_dev_audit_defaults();
    }

    // Issue #4528 AC3: cache_define's LambdaBodyWalker refuses a v1
    // callee hit whose v2 row is absent (production) — the eval /
    // cache_define-only cousin where no store_define_v2 of the callee has
    // ever run. The caller fails closed to eval_flat and no generation is
    // stored over the orphan bytes, before or after the callee mutates.
    {
        std::println("\n--- #4528 AC3: cache_define refuses an orphan v1 callee hit ---");
        apply_production_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(define z4528c (lambda () 0))").has_value(), "4528 AC3: anchor z");
        CHECK(cs.eval("(define _f4528c (lambda () 1))").has_value(), "4528 AC3: direct define f");
        CHECK(cs.has_cached_function("_f4528c"), "4528 AC3: f v1 present");
        CHECK(cs.get_define_v2("_f4528c") == nullptr,
              "4528 AC3: f has no v2 row (orphan producer)");
        cs.clear_ir_value_cells_for_test(); // #4228: reach the copy face.
        CHECK(cs.eval("(define d4528c (lambda () 0 (_f4528c)))").has_value(),
              "4528 AC3: first d define drives cache_define");
        CHECK(!cs.has_cached_function("d4528c"),
              "4528 AC3: d v1 refused — orphan v1 hit not embedded");
        // Redefine f (cache_define-only again — still no v2 row): the
        // orphan persists across the callee redefinition and the refuse
        // must persist with it — trust requires the v2 row, not a
        // fresh-looking v1.
        CHECK(cs.eval("(define _f4528c (lambda () 2))").has_value(),
              "4528 AC3: redefine f (v1 refresh, still no v2 row)");
        CHECK(cs.get_define_v2("_f4528c") == nullptr, "4528 AC3: f still has no v2 row");
        cs.clear_ir_value_cells_for_test();
        CHECK(cs.eval("(define d4528c (lambda () 7 (_f4528c)))").has_value(),
              "4528 AC3: redefine d drives cache_define");
        CHECK(!cs.has_cached_function("d4528c"), "4528 AC3: d v1 refused again after the mutation");
        CHECK(cs.get_define_v2("d4528c") == nullptr,
              "4528 AC3: no d generation stored over the orphan");
        auto call4528 = cs.eval("(d4528c)");
        CHECK(call4528 && aura::compiler::types::is_int(*call4528) &&
                  aura::compiler::types::as_int(*call4528) == 2,
              "4528 AC3: d runs the live f body, not the orphan");
        apply_dev_audit_defaults();
    }

    // Issue #4528 AC5: Soft / Off keeps the observe-only consult
    // contract — the walker consult is production-gated, so a v1 hit
    // whose v2 row is absent stays trusted under Soft.
    {
        std::println("\n--- #4528 AC5: Soft keeps the v1-hit consult off ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(define z4528e (lambda () 0))").has_value(), "4528 AC5: anchor z");
        CHECK(cs.eval("(define _f4528e (lambda () 1))").has_value(), "4528 AC5: direct define f");
        CHECK(cs.get_define_v2("_f4528e") == nullptr, "4528 AC5: f has no v2 row");
        cs.clear_ir_value_cells_for_test();
        CHECK(cs.eval("(define d4528e (lambda () 0 (_f4528e)))").has_value(),
              "4528 AC5: define d under Soft");
        CHECK(cs.has_cached_function("d4528e"),
              "4528 AC5: Soft trusts the v1 hit (observe-only consult off)");
        apply_dev_audit_defaults();
    }

    // Issue #4528 AC1: the eviction dirty arm drops the victim's
    // ir_cache_ / ir_cache_bridge_ / ir_cache_strings_ rows in the same
    // erase as ir_cache_v2_. A surviving v1 row let a later caller lower
    // copy the pre-mutate body and store_define_v2 republish it as a
    // clean generation.
    {
        std::println("\n--- #4528 AC1: dirty eviction drops the victim v1 row family ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(define z4528 (lambda () 0))").has_value(), "4528 AC1: anchor z");
        CHECK(cs.eval("(define _f4528 (lambda () 1))").has_value(), "4528 AC1: direct define f");
        CHECK(cs.has_cached_function("_f4528"), "4528 AC1: f v1 present");
        {
            aura::ir::IRFunction top4528;
            top4528.id = 0;
            top4528.name = "__top__";
            top4528.blocks.push_back({0, {}, {}});
            aura::ir::IRFunction body4528;
            body4528.id = 1;
            body4528.name = "_f4528#0";
            body4528.blocks.push_back({0, {}, {}});
            std::vector<aura::ir::IRFunction> irs4528;
            irs4528.push_back(std::move(top4528));
            irs4528.push_back(std::move(body4528));
            cs.store_define_v2("_f4528", "(define _f4528 (lambda () 1))", std::move(irs4528), {},
                               {}, cs.public_mutation_epoch());
        }
        CHECK(cs.get_define_v2("_f4528") != nullptr, "4528 AC1: f v2 row planted");
        cs.public_mark_define_dirty("_f4528");
        {
            const auto* f4528 = cs.get_define_v2("_f4528");
            CHECK(f4528 != nullptr && f4528->dirty, "4528 AC1: f dirty before the fill");
        }
        // Grow ir_cache_v2_ past kIRCacheV2MaxEntries (2048): the final
        // filler store runs the eviction, which takes the dirty victim
        // first. Only the victim is dirty, so no LRU pass runs.
        for (int i = 0; i < 2048; ++i) {
            const auto nm = "filler4528_" + std::to_string(i);
            aura::ir::IRFunction ftop;
            ftop.id = 0;
            ftop.name = "__top__";
            ftop.blocks.push_back({0, {}, {}});
            std::vector<aura::ir::IRFunction> irs;
            irs.push_back(std::move(ftop));
            cs.store_define_v2(nm, "(define " + nm + " (lambda () 0))", std::move(irs), {}, {},
                               cs.public_mutation_epoch());
        }
        CHECK(cs.get_define_v2("_f4528") == nullptr, "4528 AC1: dirty victim v2 row evicted");
        CHECK(!cs.has_cached_function("_f4528"), "4528 AC1: victim v1 dropped in the same erase");
        CHECK(cs.get_define_v2("filler4528_2047") != nullptr,
              "4528 AC1: clean filler rows survive the dirty arm");
        apply_dev_audit_defaults();
    }

    // Issue #4528 AC2: the LRU arm performs the same-generation
    // invalidate — a clean victim's v1 row goes with its v2 row.
    {
        std::println("\n--- #4528 AC2: LRU eviction drops the victim v1 row family ---");
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.eval("(define z4528b (lambda () 0))").has_value(), "4528 AC2: anchor z");
        CHECK(cs.eval("(define _l4528 (lambda () 1))").has_value(), "4528 AC2: direct define l");
        {
            aura::ir::IRFunction top4528;
            top4528.id = 0;
            top4528.name = "__top__";
            top4528.blocks.push_back({0, {}, {}});
            aura::ir::IRFunction body4528;
            body4528.id = 1;
            body4528.name = "_l4528#0";
            body4528.blocks.push_back({0, {}, {}});
            std::vector<aura::ir::IRFunction> irs4528;
            irs4528.push_back(std::move(top4528));
            irs4528.push_back(std::move(body4528));
            cs.store_define_v2("_l4528", "(define _l4528 (lambda () 1))", std::move(irs4528), {},
                               {}, cs.public_mutation_epoch());
        }
        {
            const auto* l4528 = cs.get_define_v2("_l4528");
            CHECK(l4528 != nullptr && !l4528->dirty, "4528 AC2: l planted clean");
        }
        // No dirty rows anywhere: the eviction falls through to LRU and
        // the oldest stamp is the victim planted before the fill.
        for (int i = 0; i < 2048; ++i) {
            const auto nm = "fillerl4528_" + std::to_string(i);
            aura::ir::IRFunction ftop;
            ftop.id = 0;
            ftop.name = "__top__";
            ftop.blocks.push_back({0, {}, {}});
            std::vector<aura::ir::IRFunction> irs;
            irs.push_back(std::move(ftop));
            cs.store_define_v2(nm, "(define " + nm + " (lambda () 0))", std::move(irs), {}, {},
                               cs.public_mutation_epoch());
        }
        CHECK(cs.get_define_v2("_l4528") == nullptr, "4528 AC2: LRU victim v2 row evicted");
        CHECK(!cs.has_cached_function("_l4528"),
              "4528 AC2: LRU victim v1 dropped in the same erase");
        apply_dev_audit_defaults();
    }

    // Issue #4528 AC6: source cites + no new counters + no stray issue
    // test file. Query keys and the C-bridge / defuse / table epoch are
    // untouched (the fix adds no epoch bumps and renames no keys).
    {
        const auto svc4528 = read_file("src/compiler/service.ixx");
        CHECK(read_file("tests/compiler/test_issue_4528.cpp").empty(),
              "4528 AC6: no tests/issues/test_issue_4528.cpp");
        CHECK(svc4528.find("4528_total") == std::string::npos, "4528 AC6: no new metrics counter");
        const auto e1 = svc4528.find("Issue #4528: same-generation invalidate");
        CHECK(e1 != std::string::npos, "4528 AC6: eviction cite present");
        if (e1 != std::string::npos) {
            const auto win = svc4528.substr(e1, 900);
            CHECK(win.find("ir_cache_.erase") != std::string::npos, "4528 AC6: v1 drop");
            CHECK(win.find("ir_cache_bridge_.erase") != std::string::npos, "4528 AC6: bridge drop");
            CHECK(win.find("ir_cache_strings_.erase") != std::string::npos,
                  "4528 AC6: strings drop");
        }
        const auto e2 = svc4528.find("Issue #4528: a v1 cache hit whose v2 row is absent");
        CHECK(e2 != std::string::npos, "4528 AC6: relower hit-loop cite present");
        if (e2 != std::string::npos) {
            const auto win = svc4528.substr(e2, 1000);
            CHECK(win.find("content_stored_this_epoch") != std::string::npos,
                  "4528 AC6: content latch left false");
            CHECK(win.find("embed_safe") != std::string::npos, "4528 AC6: cone exemption kept");
        }
        const auto e3 = svc4528.find("Issue #4528: a v1 hit with NO v2");
        CHECK(e3 != std::string::npos, "4528 AC6: walker consult cite present");
        if (e3 != std::string::npos) {
            const auto win = svc4528.substr(e3, 1000);
            CHECK(win.find("refused_callee_4513 = true") != std::string::npos,
                  "4528 AC6: walker refuse latched");
        }
        const auto e4 = svc4528.find("Issue #4528: distinguish a missing v2 row");
        CHECK(e4 != std::string::npos, "4528 AC6: relower scan cite present");
        if (e4 != std::string::npos) {
            const auto win = svc4528.substr(e4, 700);
            CHECK(win.find("ir_cache_v1.count") != std::string::npos,
                  "4528 AC6: scan v1 tie-break");
            CHECK(win.find("untrusted = true") != std::string::npos,
                  "4528 AC6: scan refuses the orphan");
        }
    }

    // ── Issue #4653: every want_partial == false reason must refuse the
    // per-fn commit arm on the production face ─────────────────────────
    // A workspace replace (set-code) marks every cached entry wholesale
    // dirty (mark_all_blocks_dirty).  The AoS instruction mask used to
    // stay clean (idf == 0) for those blocks, so the per-fn peel kept the
    // OLD instruction of a same-shape edit (same block count, same
    // per-block instruction count, different constant) and the per-fn
    // success arm promoted that stale content (dirty = false,
    // content_stored_this_epoch = true, live restamp) -> lookup_define_v2
    // served pre-edit IR.
    {
        std::println("\n--- #4653: production fail-closed must not promote stale IR ---");
        // Issue #4653 setup: the seed and the same-shape workspace replace
        // run on the dev face — a cold production evaluator refuses the
        // workspace swap (the #4311 member caches on the dev face for the
        // same reason).  The production face is armed for the FAIL-CLOSED
        // part: the refusal decision the rows below assert is the
        // production-face one (production_defaults_active /
        // AuditStrategy::Full).
        apply_dev_audit_defaults();
        CompilerService cs;
        // The workspace replace must use the CALL form (set-code "..."):
        // the bare symbol form never reaches the primitive, so no
        // workspace was installed and no v2 bundle was ever materialised.
        CHECK(cs.eval(R"aura((set-code "(define f4653 (lambda (x) (+ x 1)))"))aura").has_value(),
              "4653 AC1: seed define");
        CHECK(cs.eval("(eval-current)").has_value(), "4653 AC1: eval-current");
        const auto* seeded = cs.get_define_v2("f4653");
        CHECK(seeded != nullptr && !seeded->irs.empty(), "4653 AC1: v2 entry cached");
        CHECK(seeded != nullptr && seeded->irs.size() == 1, "4653 AC1: single-function bundle");
        CHECK(seeded != nullptr && seeded->content_stored_this_epoch,
              "4653 AC1: content stored for this epoch");
        CHECK(seeded != nullptr && seeded->dirty_block_count() == 0,
              "4653 AC1: stored entry has no dirty block");
        // Same-shape workspace edit: (+ x 1) -> (+ x 2).  Same block count,
        // same per-block instruction count, different constant.  The
        // replace marks every cached entry wholesale dirty — the shape the
        // fail-closed arms use.
        CHECK(cs.eval(R"aura((set-code "(define f4653 (lambda (x) (+ x 2)))"))aura").has_value(),
              "4653 AC3: same-shape edit");
        // The production face is armed from here on: the fail-closed
        // refusal + the peel that follows are the production-face ones.
        apply_production_audit_defaults();
        // AC2: a wholesale block dirty mark (the fail-closed shape every
        // want_partial == false reason uses) must cascade the AoS
        // instruction-dirty mask.  A clean (idf == 0) slot is what the
        // per-fn peel keeps as the OLD instruction of a same-shape edit.
        // mark_block_dirty_v2 with an out-of-range block takes the
        // conservative "mark the whole entry dirty" branch - the same
        // mark_all_blocks_dirty the fail-closed paths use.
        CHECK(cs.mark_block_dirty_v2("f4653", 99, 99), "4653 AC2: wholesale dirty mark");
        const auto* marked = cs.get_define_v2("f4653");
        CHECK(marked != nullptr && marked->dirty, "4653 AC2: entry dirty after the mark");
        CHECK(marked != nullptr && marked->dirty_block_count() > 0,
              "4653 AC2: blocks dirty after the mark");
        bool clean_instr_slot = false;
        if (marked != nullptr) {
            for (const auto& idf : marked->instruction_dirty_per_func_) {
                for (auto b : idf) {
                    if (b == 0)
                        clean_instr_slot = true;
                }
            }
        }
        CHECK(!clean_instr_slot,
              "4653 AC2: mark_all_blocks_dirty cascades the instruction-dirty mask");

        // #3760 / #3656: an empty source_to_ir_map is an unknown callee cone.
        // The cone consult runs BEFORE the impact consult, so the fail-closed
        // clears want_partial WITHOUT the #4422 impact refusal - the exact
        // refusal shape #4653 is about.
        CHECK(cs.public_clear_source_to_ir_map_keep_irs_for_test("f4653"),
              "4653 AC1: empty source_to_ir_map (unknown cone)");
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "4653 AC1: metrics");
        const auto full0 = m->relower_full_called_count.load(std::memory_order_relaxed);
        const auto per0 = m->relower_per_function_called_count.load(std::memory_order_relaxed);
        const auto impact0 = m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        // Drive the workspace peel (the same helper the #3484 / #3986
        // members use).  The empty source_to_ir_map is the unknown cone →
        // want_partial == false WITHOUT the #4422 impact refusal — the
        // exact refusal shape #4653 is about.
        (void)cs.public_relower_dirty_defines_from_workspace();
        // AC1: the fail-closed reason must refuse the per-fn commit arm on the
        // production face - the full arm runs instead.
        CHECK(m->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) > impact0,
              "4653 AC1: fail-closed full recorded");
        CHECK(m->relower_full_called_count.load(std::memory_order_relaxed) > full0,
              "4653 AC1: the full arm ran");
        CHECK(m->relower_per_function_called_count.load(std::memory_order_relaxed) == per0,
              "4653 AC1: the per-fn commit arm was refused");
        // AC3: the new constant must be in the IR, or the entry must stay
        // dirty (lookup == 1).  A clean hit on the pre-edit IR is the #4653
        // defect.
        const auto* after = cs.get_define_v2("f4653");
        CHECK(after != nullptr, "4653 AC3: entry after the peel");
        bool new_const = false;
        bool old_const = false;
        if (after != nullptr) {
            for (const auto& fn : after->irs) {
                for (const auto& b : fn.blocks) {
                    for (const auto& ins : b.instructions) {
                        if (ins.opcode != aura::ir::IROpcode::ConstI64)
                            continue;
                        if (ins.operands[1] == 2)
                            new_const = true;
                        if (ins.operands[1] == 1)
                            old_const = true;
                    }
                }
            }
        }
        const int look = after != nullptr ? cs.lookup_define_v2("f4653", after->source_hash) : 2;
        std::println("  4653: new_const={} old_const={} lookup={}", new_const, old_const, look);
        CHECK(new_const || look == 1,
              "4653 AC3: the same-shape edit is not served from the stale IR");
        apply_dev_audit_defaults();
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
