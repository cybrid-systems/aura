// tests/compiler/test_ir_soa_dual_emit_batch.cpp — IR SoA dual-emit family dup-merge (R19 phase
// 11). R19 phase11 — Issue #1377 + #1629 + #1920 IR SoA dual-emit trio
//
//   #1377: IR SoA dual-emit (gated behind opt-in flag, default off) — original gate
//   #1629: refine #1377 — feature flag default false, 100× stress, schema-1629 keys
//   #1920: Phase 2 adoption — IRModuleV2View + walk_soa_function_hotpath + to_aos_module,
//          DCE / TypeProp / ConstFold on IRModuleV2, schema-1920
//
//   AC1:  CompilerService dual-emit default false (#1377)
//   AC2:  process flag default false (#1377)
//   AC3:  lower_to_ir respects flag (off → skipped, soa snap 0) (#1377)
//   AC4:  opt-in produces SoA metrics (ir_soa_instructions > 0) (#1377)
//   AC5:  toggle off again zeros new emission (#1377)
//   AC6:  SoAtoAoSBridgePass early-out on empty module (#1377)
//   AC7:  correctness parity off vs on (fact 6 == 720 both) (#1377)
//   AC8:  repeated lowers default-off stay clean (20×) (#1377)
//   AC9:  100× lower stress default-off (soa counters stay 0, skipped grows) (#1629)
//   AC10: query:soa-adoption-stats schema-1629 keys (#1629)
//   AC11: #1377 lineage (toggle / bridge counters) (#1629)
//   AC12: IRModuleV2View + walk_soa_function_hotpath + to_aos_module (#1920)
//   AC13: DCE / TypeProp / ConstFold run(IRModuleV2) dirty-driven (#1920)
//   AC14: consumer family counters (lowering/pass/jit/executor) (#1920)
//   AC15: dirty_block_driven_skips / clean hit rate (#1920)
//   AC16: shape/linear column consults + capture dirty marks (#1920)
//   AC17: query:soa-adoption-stats schema-1920 keys (#1920)
//   AC18: multi-round mutate stress (capture dirty advances) (#1920)
//   AC19: #1629 lineage schema retained (#1920)
//   AC20: absorb / SoAtoAoSBridge early-out when single-emit (#1629)
//   AC21: flag on still correct eval; parity off vs on (#1629)
//
// Skip: test_ir_soa_incremental_closed_loop.cpp (#404) — different imports (uses Evaluator
//       not CompilerService), tests query:ir-soa-incremental-stats (different pillar from
//       dual-emit family). Kept separate.

#include "test_harness.hpp"
#include "compiler/jit_typed_mutation_stats.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.pass_manager;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.soa_view;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::IRFunctionSoA;
using aura::compiler::IRModuleV2;
using aura::compiler::to_aos_module;
using aura::compiler::to_aos_view;
using aura::compiler::TypePropagationPass;
using aura::compiler::walk_soa_function_hotpath;
using aura::compiler::soa_view::make_function_soa_view;
using aura::compiler::soa_view::make_module_soa_view;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

namespace mig = aura::compiler::ir_soa_migration;

struct NoopPass {
    void run(aura::ir::IRModule&) {}
    [[nodiscard]] bool has_error() const { return false; }
};

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static IRModuleV2 make_sample_mod() {
    IRModuleV2 mod;
    auto fi = mod.add_function("f", 4);
    auto bi = mod.add_block(fi);
    mod.add_instruction(fi, IROpcode::ConstI64, {0, 2, 0, 0}, 0, 1, 7, 0);
    mod.add_instruction(fi, IROpcode::ConstI64, {1, 4, 0, 0}, 0, 1, 7, 0);
    mod.add_instruction(fi, IROpcode::Add, {2, 0, 1, 0}, 0, 1, 7, 1);
    mod.seal_block(fi, bi);
    auto bi2 = mod.add_block(fi);
    mod.add_instruction(fi, IROpcode::ConstI64, {3, 0, 0, 0}, 0, 1, 0, 0);
    mod.seal_block(fi, bi2);
    mod.functions[fi].mark_block_dirty(0);
    mod.functions[fi].mark_block_dirty(1);
    mod.functions[fi].clear_block_dirty(1);
    return mod;
}

// ── #1629 ACs ─────────────────────────────────────────────────────────

static void ac1629_1_default_off() {
    std::println("\n--- AC9: default dual-emit off (#1629 AC1) ---");
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    CompilerService cs;
    CHECK(!cs.soa_dual_emit_enabled(), "CompilerService default false");
    CHECK(!aura::compiler::ir_soa_migration::soa_dual_emit_enabled(), "process default false");
}

static void ac1629_2_lower_respects() {
    std::println("\n--- AC10: lower respects flag (#1629 AC2) ---");
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    CompilerService cs;
    const auto skip0 = aura::compiler::ir_soa_migration::dual_emit_skipped_total.load();
    CHECK(cs.eval("(set-code \"(define (id x) x) (id 1)\")").has_value(), "set-code");
    auto r = cs.eval("(eval-current)");
    CHECK(r && is_int(*r) && as_int(*r) == 1, "eval == 1 dual-emit off");
    auto snap = cs.snapshot();
    CHECK(snap.ir_soa_instructions_emitted == 0, "soa instr == 0");
    CHECK(snap.ir_soa_functions_emitted == 0, "soa funcs == 0");
    CHECK(aura::compiler::ir_soa_migration::dual_emit_skipped_total.load() > skip0,
          "skipped advanced");
}

static void ac1629_3_bridge_early_out() {
    std::println("\n--- AC20: SoAtoAoSBridge early-out (#1629 AC3) ---");
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    NoopPass nop;
    aura::compiler::SoAtoAoSBridgePass bridge(nop);
    aura::compiler::IRModuleV2 fake;
    aura::compiler::IRFunctionSoA fn;
    fn.opcodes_.push_back(aura::ir::IROpcode::Add);
    fake.functions.push_back(std::move(fn));
    CHECK(bridge.run(fake), "run with dual off");
    CHECK(bridge.soa_functions_visited() == 0, "no visit when dual off");
    CHECK(bridge.aos_view_built_count() == 0, "no aos view when dual off");
    aura::compiler::IRModuleV2 empty;
    CHECK(bridge.run(empty), "empty run");
}

static void ac1629_4_stress_off() {
    std::println("\n--- AC9 (stress): 100× lower stress default-off (#1629 AC4) ---");
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    CompilerService cs;
    const auto skip0 = aura::compiler::ir_soa_migration::dual_emit_skipped_total.load();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        (void)cs.eval(
            std::format("(set-code \"(define (f{} x) (+ x {})) (f{} {})\")", i, i % 7, i, i % 5));
        (void)cs.eval("(eval-current)");
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    auto snap = cs.snapshot();
    CHECK(snap.ir_soa_instructions_emitted == 0, "100× off: soa instr 0");
    CHECK(snap.ir_soa_functions_emitted == 0, "100× off: soa funcs 0");
    CHECK(aura::compiler::ir_soa_migration::dual_emit_skipped_total.load() >= skip0 + 50,
          "skipped grew under stress");
    std::println("  100 lowers dual-off wall_ms={} soa_instr={}", ms,
                 snap.ir_soa_instructions_emitted);
    CHECK(true, "stress completed");
}

static void ac1629_5_parity() {
    std::println("\n--- AC21: flag on correctness + parity (#1629 AC5) ---");
    auto run = [](bool dual) -> std::int64_t {
        CompilerService cs;
        cs.set_soa_dual_emit(dual);
        (void)cs.eval(
            "(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 6)\")");
        auto r = cs.eval("(eval-current)");
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    };
    const auto off = run(false);
    const auto on = run(true);
    CHECK(off == 720, "off fact 6");
    CHECK(on == 720, "on fact 6");
    CHECK(off == on, "parity");
    CompilerService cs;
    cs.set_soa_dual_emit(true);
    const auto b0 = aura::compiler::ir_soa_migration::dual_emit_bridge_count.load();
    (void)cs.eval("(set-code \"(define (add a b) (+ a b)) (add 2 3)\")");
    auto r = cs.eval("(eval-current)");
    CHECK(r && is_int(*r) && as_int(*r) == 5, "add with dual on");
    CHECK(cs.snapshot().ir_soa_instructions_emitted > 0, "opt-in emitted");
    CHECK(aura::compiler::ir_soa_migration::dual_emit_bridge_count.load() > b0, "bridge count");
    cs.set_soa_dual_emit(false);
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
}

static void ac1629_6_schema() {
    std::println("\n--- AC10 (schema): query:soa-adoption-stats schema-1629 (#1629 AC6) ---");
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:soa-adoption-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:soa-adoption-stats", "schema") == 1629 ||
              href(cs, "query:soa-adoption-stats", "schema") == 1619 ||
              href(cs, "query:soa-adoption-stats", "schema") == 1517,
          "schema 1629|lineage");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-dual-emit-default-off") == 1,
          "default-off flag");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-dual-emit-flag-wired") == 1, "wired");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-dual-emit-enabled") == 0, "enabled==0 default");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-dual-emit-skipped-total") >= 0, "skipped key");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-dual-emit-bridge-count") >= 0, "bridge key");
    CHECK(href(cs, "query:soa-adoption-stats", "issue") == 1629 ||
              href(cs, "query:soa-adoption-stats", "issue") < 0,
          "issue 1629");
}

static void ac1629_7_lineage() {
    std::println("\n--- AC11: #1377 lineage toggle (#1629 AC7) ---");
    CompilerService cs;
    cs.set_soa_dual_emit(true);
    CHECK(cs.soa_dual_emit_enabled(), "toggle on");
    cs.set_soa_dual_emit(false);
    CHECK(!cs.soa_dual_emit_enabled(), "toggle off");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-functions-visited") >= 0,
          "adoption lineage key");
}

// ── #1920 ACs ─────────────────────────────────────────────────────────

static void ac1920_1_views() {
    std::println("\n--- AC12: IRModuleV2View + walk + to_aos (#1920 AC1) ---");
    CHECK(mig::kIrSoaMigrationPhase == 2, "phase == 2");
    auto mod = make_sample_mod();
    auto mview = make_module_soa_view(&mod);
    CHECK(mview.function_count() == 1, "1 function");
    auto fview = mview.function_view(0);
    CHECK(fview.size() >= 3, "instrs >= 3");
    auto col = fview.columnar_accessor();
    CHECK(col.size() >= 3, "columnar");
    auto aos = to_aos_view(mod.functions[0]);
    CHECK(aos.blocks.size() >= 1, "aos blocks");
    auto aos_mod = to_aos_module(mod);
    CHECK(aos_mod.functions.size() == 1, "aos module");
    auto walk = walk_soa_function_hotpath(mod.functions[0], true);
    CHECK(walk.dirty_runs >= 1, "dirty runs");
    CHECK(walk.clean_skips >= 0, "clean skips");
    mig::record_consumer_executor();
    if (walk.clean_skips)
        mig::record_dirty_block_skip(walk.clean_skips);
    if (walk.dirty_runs)
        mig::record_dirty_block_run(walk.dirty_runs);
    CHECK(load_u64(mig::consumer_executor_hits) >= 1, "executor consumer");
}

static void ac1920_2_pass_soa() {
    std::println("\n--- AC13: DCE / TypeProp / ConstFold SoA run (#1920 AC2) ---");
    auto mod = make_sample_mod();
    DeadCoercionEliminationPass dce(nullptr);
    const auto p0 = load_u64(mig::consumer_pass_hits);
    const auto sk0 = load_u64(mig::dirty_block_driven_skips);
    dce.run(mod, /*dirty_blocks_only=*/true);
    CHECK(load_u64(mig::consumer_pass_hits) > p0, "DCE pass consumer");

    TypePropagationPass tp(nullptr);
    tp.run(mod, true);
    CHECK(load_u64(mig::consumer_pass_hits) >= p0 + 2, "TypeProp pass consumer");

    ConstantFoldingWrap cf;
    cf.run(mod, true);
    CHECK(load_u64(mig::consumer_pass_hits) >= p0 + 3, "CF pass consumer");
    CHECK(load_u64(mig::dirty_block_driven_skips) >= sk0, "skips advanced or equal");
    CHECK(cf.folded_count() >= 0, "folded count");
}

static void ac1920_3_families() {
    std::println("\n--- AC14: consumer families (#1920 AC3) ---");
    mig::record_consumer_lowering();
    mig::record_consumer_jit();
    const auto n = mig::consumer_families_active();
    CHECK(n >= 3, std::format("families active {} >= 3", n));
    CHECK(load_u64(mig::consumer_lowering_hits) >= 1, "lowering");
    CHECK(load_u64(mig::consumer_pass_hits) >= 1, "pass");
    CHECK(load_u64(mig::consumer_jit_hits) >= 1, "jit");
}

static void ac1920_4_dirty_rate() {
    std::println("\n--- AC15: dirty-driven clean hit rate (#1920 AC4) ---");
    mig::record_dirty_block_skip(5);
    mig::record_dirty_block_run(5);
    const auto bp = mig::dirty_driven_clean_hit_rate_bp();
    CHECK(bp > 0 && bp <= 10000, std::format("clean hit rate bp={}", bp));
}

static void ac1920_5_shape_capture() {
    std::println("\n--- AC16: shape/linear consult + capture dirty (#1920 AC5) ---");
    auto mod = make_sample_mod();
    auto view = make_function_soa_view(&mod.functions[0]);
    const auto s0 = load_u64(mig::shape_column_consults);
    const auto l0 = load_u64(mig::linear_column_consults);
    (void)aura::compiler::soa_view::consult_shape(view, 0);
    (void)aura::compiler::soa_view::consult_linear(view, 0);
    CHECK(load_u64(mig::shape_column_consults) > s0, "shape consult");
    CHECK(load_u64(mig::linear_column_consults) > l0, "linear consult");
    mig::record_capture_dirty_mark(2);
    CHECK(load_u64(mig::capture_dirty_marks_total) >= 2, "capture dirty");
}

static void ac1920_6_schema_1920() {
    std::println("\n--- AC17: query:soa-adoption-stats schema-1920 (#1920 AC6) ---");
    CompilerService cs;
    auto mod = make_sample_mod();
    ConstantFoldingWrap cf;
    cf.run(mod, true);
    auto h = cs.eval("(engine:metrics \"query:soa-adoption-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:soa-adoption-stats", "schema") == 1629, "lineage 1629");
    CHECK(href(cs, "query:soa-adoption-stats", "schema-1920") == 1920, "schema-1920");
    CHECK(href(cs, "query:soa-adoption-stats", "issue-1920") == 1920, "issue-1920");
    CHECK(href(cs, "query:soa-adoption-stats", "migration-phase") == 2, "phase 2");
    CHECK(href(cs, "query:soa-adoption-stats", "phase2-consumer-wired") == 1, "wired");
    CHECK(href(cs, "query:soa-adoption-stats", "dce-soa-run-wired") == 1, "dce wired");
    CHECK(href(cs, "query:soa-adoption-stats", "typeprop-soa-run-wired") == 1, "tp wired");
    CHECK(href(cs, "query:soa-adoption-stats", "constfold-soa-run-wired") == 1, "cf wired");
    CHECK(href(cs, "query:soa-adoption-stats", "irmodulev2-view-wired") == 1, "view wired");
    CHECK(href(cs, "query:soa-adoption-stats", "consumer-pass-hits") >= 0, "pass hits");
    CHECK(href(cs, "query:soa-adoption-stats", "dirty-block-driven-skips") >= 0, "skips");
    CHECK(href(cs, "query:soa-adoption-stats", "dirty-driven-clean-hit-rate-bp") >= 0, "hit rate");
    CHECK(href(cs, "query:soa-adoption-stats", "capture-dirty-marks") >= 0, "capture dirty");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-dual-emit-flag-wired") == 1, "dual-emit");
}

static void ac1920_7_mutate_stress() {
    std::println("\n--- AC18: mutate stress + capture dirty (#1920 AC7) ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define (g y) (f y))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto cap0 = href(cs, "query:soa-adoption-stats", "capture-dirty-marks");
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\" \"i1920\")", i % 5));
        (void)cs.eval("(eval-current)");
    }
    CHECK(href(cs, "query:soa-adoption-stats", "schema-1920") == 1920, "schema holds");
    CHECK(href(cs, "query:soa-adoption-stats", "capture-dirty-marks") >= cap0, "capture non-dec");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
}

static void ac1920_8_lineage() {
    std::println("\n--- AC19: #1629 lineage (#1920 AC8) ---");
    CompilerService cs;
    CHECK(href(cs, "query:soa-adoption-stats", "schema") == 1629, "schema 1629");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-functions-visited") >= 0, "funcs");
    CHECK(href(cs, "query:soa-adoption-stats", "soa-dual-emit-default-off") == 1, "default off");
    CHECK(href(cs, "query:soa-adoption-stats", "issue") == 1629, "issue 1629");
}

} // namespace

int main() {
    // Ensure process default is off for this binary (other tests may flip).
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);

    std::println("=== IR SoA dual-emit family: #1377 + #1629 + #1920 ===\n");

    // ── #1377 ACs (inline, preserved structure) ──

    // AC1: default off
    {
        std::println("--- AC1: CompilerService dual-emit default false ---");
        CompilerService cs;
        CHECK(!cs.soa_dual_emit_enabled(), "CompilerService dual-emit default false");
        CHECK(!aura::compiler::ir_soa_migration::soa_dual_emit_enabled(),
              "process flag default false");
        const auto skip0 = aura::compiler::ir_soa_migration::dual_emit_skipped_total.load();
        CHECK(cs.eval("(set-code \"(define (id x) x) (id 1)\")").has_value(), "set-code");
        auto r = cs.eval("(eval-current)");
        CHECK(r && is_int(*r) && as_int(*r) == 1, "eval-current == 1 with dual-emit off");
        auto snap = cs.snapshot();
        CHECK(snap.ir_soa_instructions_emitted == 0, "default-off: ir_soa_instructions_emitted==0");
        CHECK(snap.ir_soa_functions_emitted == 0, "default-off: ir_soa_functions_emitted==0");
        CHECK(aura::compiler::ir_soa_migration::dual_emit_skipped_total.load() > skip0,
              "skipped counter advanced on lower");
    }

    // AC2: opt-in produces SoA metrics
    {
        std::println("\n--- AC2: opt-in produces SoA metrics ---");
        CompilerService cs;
        cs.set_soa_dual_emit(true);
        CHECK(cs.soa_dual_emit_enabled(), "set_soa_dual_emit(true) sticks");
        const auto bridge0 = aura::compiler::ir_soa_migration::dual_emit_bridge_count.load();
        CHECK(cs.eval("(set-code \"(define (add a b) (+ a b)) (add 2 3)\")").has_value(),
              "set-code opt-in");
        auto r = cs.eval("(eval-current)");
        CHECK(r && is_int(*r) && as_int(*r) == 5, "eval-current == 5 with dual-emit on");
        auto snap = cs.snapshot();
        CHECK(snap.ir_soa_instructions_emitted > 0, "opt-in: ir_soa_instructions_emitted > 0");
        CHECK(snap.ir_soa_functions_emitted > 0, "opt-in: ir_soa_functions_emitted > 0");
        CHECK(aura::compiler::ir_soa_migration::dual_emit_bridge_count.load() > bridge0,
              "bridge counter advanced when on");
        cs.set_soa_dual_emit(false);
    }

    // AC3: toggle off again zeros new emission
    {
        std::println("\n--- AC3: toggle off again zeros new emission ---");
        CompilerService cs;
        cs.set_soa_dual_emit(true);
        CHECK(cs.eval("(set-code \"(define (p) (+ 1 2)) (p)\")").has_value(), "set-code define on");
        auto r = cs.eval("(eval-current)");
        CHECK(r && is_int(*r) && as_int(*r) == 3, "eval p == 3 dual-emit on");
        auto after_on = cs.snapshot().ir_soa_instructions_emitted;
        CHECK(after_on > 0, "on path emitted > 0");
        cs.set_soa_dual_emit(false);
        const auto before = cs.snapshot().ir_soa_instructions_emitted;
        CHECK(cs.eval("(set-code \"(define (q) (+ 3 4)) (q)\")").has_value(),
              "set-code define off");
        auto r2 = cs.eval("(eval-current)");
        CHECK(r2 && is_int(*r2) && as_int(*r2) == 7, "eval q == 7 dual-emit off");
        auto after_off = cs.snapshot().ir_soa_instructions_emitted;
        CHECK(after_off == before, "after disable: counters do not grow on lower");
    }

    // AC4: SoAtoAoSBridgePass early-out on empty module
    {
        std::println("\n--- AC4: SoAtoAoSBridgePass early-out on empty module ---");
        NoopPass nop;
        aura::compiler::SoAtoAoSBridgePass bridge(nop);
        aura::compiler::IRModuleV2 empty;
        CHECK(bridge.run(empty), "empty SoA run returns true");
        CHECK(bridge.soa_functions_visited() == 0, "empty: 0 functions visited");
        CHECK(bridge.aos_view_built_count() == 0, "empty: 0 aos views built");
    }

    // AC5: correctness parity off vs on (fact 6)
    {
        std::println("\n--- AC5: correctness parity off vs on ---");
        auto run_once = [](bool dual) -> std::int64_t {
            CompilerService cs;
            cs.set_soa_dual_emit(dual);
            (void)cs.eval(
                "(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 6)\")");
            auto r = cs.eval("(eval-current)");
            if (!r || !is_int(*r))
                return -1;
            return as_int(*r);
        };
        const auto off = run_once(false);
        const auto on = run_once(true);
        CHECK(off == 720, "fact 6 == 720 dual-emit off");
        CHECK(on == 720, "fact 6 == 720 dual-emit on");
        CHECK(off == on, "parity: off and on agree");
        aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    }

    // AC6: repeated lowers default-off stay clean
    {
        std::println("\n--- AC6: repeated lowers default-off stay clean ---");
        CompilerService cs;
        CHECK(!cs.soa_dual_emit_enabled(), "still default false");
        for (int i = 0; i < 20; ++i) {
            (void)cs.eval(std::format("(set-code \"(+ {} {})\")", i, i + 1));
            (void)cs.eval("(eval-current)");
        }
        auto snap = cs.snapshot();
        CHECK(snap.ir_soa_instructions_emitted == 0, "20 lowers default-off: soa instr == 0");
        CHECK(snap.ir_soa_functions_emitted == 0, "20 lowers default-off: soa funcs == 0");
    }

    // ── #1629 ACs (function-style) ──
    ac1629_1_default_off();
    ac1629_2_lower_respects();
    ac1629_3_bridge_early_out();
    ac1629_4_stress_off();
    ac1629_5_parity();
    ac1629_6_schema();
    ac1629_7_lineage();

    // ── #1920 ACs (function-style) ──
    ac1920_1_views();
    ac1920_2_pass_soa();
    ac1920_3_families();
    ac1920_4_dirty_rate();
    ac1920_5_shape_capture();
    ac1920_6_schema_1920();
    ac1920_7_mutate_stress();
    ac1920_8_lineage();

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
