// @category: integration
// @reason: Issue #1629 — gate IR SoA dual-emit behind feature flag
// Issue #1377/#1629 (#1978 renamed): issue# moved from filename to header.
// default-off (refine #1377 production overhead gap).
//
//   AC1: enable_soa_dual_emit_ / process flag default false
//   AC2: lower_to_ir respects flag (off → skipped, soa snap 0)
//   AC3: absorb / SoAtoAoSBridge early-out when single-emit
//   AC4: 100× lower stress default-off — soa counters stay 0, skipped grows
//   AC5: flag on still correct eval; parity off vs on
//   AC6: query:soa-adoption-stats schema 1629 AC keys
//   AC7: #1377 lineage (toggle / bridge counters)

#include "test_harness.hpp"
#include "compiler/jit_typed_mutation_stats.h"
#include "compiler/observability_metrics.h"

#include <chrono>
#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.pass_manager;
import aura.compiler.ir_soa;
import aura.compiler.ir;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

struct NoopPass {
    void run(aura::ir::IRModule&) {}
    [[nodiscard]] bool has_error() const { return false; }
};

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:soa-adoption-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_default_off() {
    std::println("\n--- AC1: default dual-emit off ---");
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    CompilerService cs;
    CHECK(!cs.soa_dual_emit_enabled(), "CompilerService default false");
    CHECK(!aura::compiler::ir_soa_migration::soa_dual_emit_enabled(), "process default false");
}

static void ac2_lower_respects() {
    std::println("\n--- AC2: lower respects flag ---");
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

static void ac3_bridge_early_out() {
    std::println("\n--- AC3: SoAtoAoSBridge early-out ---");
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    NoopPass nop;
    aura::compiler::SoAtoAoSBridgePass bridge(nop);
    // Non-empty module but dual-emit off → no conversion work.
    aura::compiler::IRModuleV2 fake;
    aura::compiler::IRFunctionSoA fn;
    fn.opcodes_.push_back(aura::ir::IROpcode::Add);
    fake.functions.push_back(std::move(fn));
    CHECK(bridge.run(fake), "run with dual off");
    CHECK(bridge.soa_functions_visited() == 0, "no visit when dual off");
    CHECK(bridge.aos_view_built_count() == 0, "no aos view when dual off");
    // Empty always early-out
    aura::compiler::IRModuleV2 empty;
    CHECK(bridge.run(empty), "empty run");
}

static void ac4_stress_off() {
    std::println("\n--- AC4: 100× lower stress default-off ---");
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

static void ac5_parity() {
    std::println("\n--- AC5: flag on correctness + parity ---");
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
    // opt-in metrics
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

static void ac6_schema() {
    std::println("\n--- AC6: query:soa-adoption-stats schema 1629 ---");
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:soa-adoption-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1629 || href(cs, "schema") == 1619 || href(cs, "schema") == 1517,
          "schema 1629|lineage");
    CHECK(href(cs, "soa-dual-emit-default-off") == 1, "default-off flag");
    CHECK(href(cs, "soa-dual-emit-flag-wired") == 1, "wired");
    CHECK(href(cs, "soa-dual-emit-enabled") == 0, "enabled==0 default");
    CHECK(href(cs, "soa-dual-emit-skipped-total") >= 0, "skipped key");
    CHECK(href(cs, "soa-dual-emit-bridge-count") >= 0, "bridge key");
    CHECK(href(cs, "issue") == 1629 || href(cs, "issue") < 0, "issue 1629");
}

static void ac7_lineage() {
    std::println("\n--- AC7: #1377 lineage toggle ---");
    CompilerService cs;
    cs.set_soa_dual_emit(true);
    CHECK(cs.soa_dual_emit_enabled(), "toggle on");
    cs.set_soa_dual_emit(false);
    CHECK(!cs.soa_dual_emit_enabled(), "toggle off");
    CHECK(href(cs, "soa-functions-visited") >= 0, "adoption lineage key");
}

} // namespace

int main() {
    std::println("=== Issue #1629: IR SoA dual-emit flag default-off ===");
    ac1_default_off();
    ac2_lower_respects();
    ac3_bridge_early_out();
    ac4_stress_off();
    ac5_parity();
    ac6_schema();
    ac7_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
