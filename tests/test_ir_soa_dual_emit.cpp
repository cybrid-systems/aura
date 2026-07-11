// test_ir_soa_dual_emit.cpp — Issue #1377:
// Gate IR SoA dual-emit behind opt-in flag (default off).

#include "test_harness.hpp"

#include <cstdint>
#include <string>

#include "jit_typed_mutation_stats.h"

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.pass_manager;
import aura.compiler.ir_soa;
import aura.compiler.ir;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

namespace {

struct NoopPass {
    void run(aura::ir::IRModule&) {}
    [[nodiscard]] bool has_error() const { return false; }
};

} // namespace

int main() {
    // Ensure process default is off for this binary (other tests may flip).
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);

    // ── AC1: default off ──
    {
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

    // ── AC2: opt-in produces SoA metrics ──
    {
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

    // ── AC3: toggle off again zeros new emission ──
    {
        CompilerService cs;
        cs.set_soa_dual_emit(true);
        // Use a define body so production lower_to_ir_with_cache runs
        // (bare literals may short-circuit without dual-emit absorb).
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

    // ── AC4: SoAtoAoSBridgePass early-out on empty module ──
    {
        NoopPass nop;
        aura::compiler::SoAtoAoSBridgePass bridge(nop);
        aura::compiler::IRModuleV2 empty;
        CHECK(bridge.run(empty), "empty SoA run returns true");
        CHECK(bridge.soa_functions_visited() == 0, "empty: 0 functions visited");
        CHECK(bridge.aos_view_built_count() == 0, "empty: 0 aos views built");
    }

    // ── AC5: correctness parity off vs on ──
    {
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

    // ── AC6: repeated lowers default-off stay clean ──
    {
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

    if (::aura::test::g_failed)
        return 1;
    std::println("ir_soa dual-emit gate #1377: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
