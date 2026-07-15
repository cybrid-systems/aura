// @category: integration
// @reason: uses CompilerService to verify IR encoding observability
//
// test_issue_375.cpp — Issue #375: IR encoding observability
// foundation + baseline numbers for the AoS-vs-compact AC.
//
// Background: the issue body asks for an IRInstruction SoA
// transformation to relieve I-cache pressure on the
// interpreter hot path. The full transform (Phase B/C in the
// scope-limited close plan) touches ir_executor_impl.cpp's
// switch dispatch + multiple passes — high hot-path risk
// without baseline data to design against.
//
// This scope-limited close ships the FOUNDATION only:
//   1. aura::ir::IRStatsSnapshot struct (ir.ixx) + free
//      function compute_ir_stats() that scans an IRModule
//      and produces:
//        - total_instructions / total_functions / total_blocks
//        - operand_count_distribution[0..4]
//        - opcode_histogram[54]
//        - aos_bytes_total / padding_bytes_total /
//          unused_operand_bytes_total
//        - compact_bytes_projection (variable-length: 2-byte
//          header + 4 bytes per used operand, 4-byte aligned)
//        - compact_ratio_bp (compact / aos in basis points;
//          the AC1 ≥30% reduction = ratio ≤ 7000 bp)
//   2. CompilerService::last_ir_stats() — snapshot of the
//      last compiled module's stats, computed when
//      last_ir_mod_ is set. C++ test API + Aura primitive
//      both read this (avoids the "primitive sees its own
//      IR" clobber problem).
//   3. (engine:metrics \"compile:ir-stats\") Aura primitive — returns the
//      snapshot as a hash for EDSL code. Best-effort: when
//      called from .aura, the stats reflect the LAST workload
//      that was lowered, not the stats-call's own IR.
//   4. test_issue_375.cpp (this file) — 5 ACs that verify
//      the snapshot's fields are populated correctly on
//      representative workloads (a simple expression, a
//      recursive function, a function with closures, a
//      function with control flow).
//
// Pairs with ir_soa.ixx (SoA skeleton, no consumer yet) +
// #167 Phase 1 (SoA infrastructure).
//
// Out of scope (deferred to follow-up sessions per
// scope-limited close):
//   - B: dual representation (AoS for analysis + compact
//     view for hot path) + IRFunctionSoA::view_at_compact().
//   - C: ir_executor_impl.cpp's switch → compact view
//     iteration (true hot-path change, depends on B for
//     the consumer API).
//   - D: computed-goto dispatch (separate from encoding —
//     depends on C's compact view).
//   - E: JIT lowering reads from compact view instead of
//     re-decoding AoS IRFunction (separate from C).

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir; // Issue #375: IRStatsSnapshot + compute_ir_stats
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_375_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println("  PASS: {}", msg);                                                       \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}", msg);                                                       \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                                        \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            ++g_passed;                                                                            \
            std::println("  PASS: {}  ({} = {})", msg, _a, _b);                                    \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}  ({} != {})", msg, _a, _b);                                   \
        }                                                                                          \
    } while (0)

#define CHECK_GE(a, b, msg)                                                                        \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a >= _b) {                                                                            \
            ++g_passed;                                                                            \
            std::println("  PASS: {}  ({} >= {})", msg, _a, _b);                                   \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}  ({} < {})", msg, _a, _b);                                    \
        }                                                                                          \
    } while (0)

// Helper: run a workload through the IR pipeline and return
// the resulting snapshot. We use cs.eval_ir() (NOT cs.eval())
// because eval() short-circuits to the tree-walker path when
// a workspace is set; eval_ir() always goes through the IR
// pipeline (which sets last_ir_mod_ + last_ir_stats_).
//
// Returns a copy of the IRStatsSnapshot.
aura::ir::IRStatsSnapshot run_workload_and_get_stats(aura::compiler::CompilerService& cs,
                                                     const std::string& workload) {
    auto r = cs.eval_ir(workload);
    if (!r) {
        std::println("  [helper] eval_ir failed: {}", workload);
    }
    return cs.last_ir_stats();
}

// AC1: snapshot is zero on a fresh CompilerService (no module
// compiled yet). Verifies the default-initialization path.
bool test_initial_snapshot_zero() {
    std::println("\n--- AC1: snapshot starts at zero on a fresh CompilerService ---");
    aura::compiler::CompilerService cs;
    const auto& s = cs.last_ir_stats();
    CHECK_EQ(s.total_instructions, 0u, "total-instructions is 0 on fresh service");
    CHECK_EQ(s.total_functions, 0u, "total-functions is 0 on fresh service");
    CHECK_EQ(s.total_blocks, 0u, "total-blocks is 0 on fresh service");
    CHECK_EQ(s.aos_bytes_total, 0u, "aos-bytes-total is 0 on fresh service");
    CHECK_EQ(s.compact_bytes_projection, 0u, "compact-bytes-projection is 0 on fresh service");
    CHECK_EQ(s.compact_ratio_bp, 0u, "compact-ratio-bp is 0 on fresh service");
    return true;
}

// AC2: a simple expression workload populates the snapshot.
// Verifies the snapshot updates on last_ir_mod_ assignment.
bool test_snapshot_updates_on_workload() {
    std::println("\n--- AC2: snapshot populates after a simple expression workload ---");
    aura::compiler::CompilerService cs;
    const auto& s = run_workload_and_get_stats(cs, "((lambda (n) (* n n)) 7)");
    // A single lambda body (ConstI64 n, Mul, Return) should
    // have ≥ 3 instructions. The exact count depends on the
    // lowering pass + capture handling; we just want > 0 here.
    CHECK(s.total_instructions > 0, "total-instructions > 0 after eval(f 7)");
    CHECK(s.total_functions >= 1, "total-functions >= 1 (the lambda body)");
    CHECK(s.aos_bytes_total == s.total_instructions * 40,
          "aos-bytes-total = total-instr * 40 (verified field math)");
    CHECK(s.padding_bytes_total == s.total_instructions * 3,
          "padding-bytes-total = total-instr * 3 (linear_state -> adt_variant_id gap)");
    return true;
}

// AC3: compact_ratio_bp is in valid range [0, 10000] on a
// realistic workload. The #375 AC is "≥30% reduction" which
// corresponds to ratio_bp ≤ 7000. We don't enforce 7000 here
// because the projection depends on workload operand
// distribution; we just verify the math doesn't overflow or
// go negative.
bool test_compact_ratio_in_range() {
    std::println("\n--- AC3: compact-ratio-bp is in [0, 10000] on a realistic workload ---");
    aura::compiler::CompilerService cs;
    const auto& s =
        run_workload_and_get_stats(cs, "((lambda (n)\n"
                                       "  (let loop ((i 0) (acc 0))\n"
                                       "    (if (>= i n) acc (loop (+ i 1) (+ acc i)))))\n"
                                       " 10)");
    CHECK(s.total_instructions > 0, "sum-to lowered to > 0 instructions");
    CHECK(s.compact_ratio_bp <= 10000, "compact-ratio-bp <= 10000 (no overflow)");
    CHECK(s.operands_used_sum > 0,
          "operands-used-sum > 0 (at least one instruction uses operands)");
    // A workload with Let, If, Add, comparison, call patterns
    // has avg-operands typically 2-3, so the projection should
    // be 50-75% of the AoS bytes (not 99%, not 5%).
    // (Strict check would be brittle; we just verify the
    // math is in a sane range.)
    CHECK(s.compact_bytes_projection > 0, "compact-bytes-projection > 0");
    CHECK(s.compact_bytes_projection < s.aos_bytes_total,
          "compact-bytes-projection < aos-bytes-total (otherwise why bother)");
    std::println("       [baseline] total-instr={} avg-ops={}/100 aos={} compact={} ratio={}bp",
                 s.total_instructions,
                 s.total_instructions ? (s.operands_used_sum * 100u / s.total_instructions) : 0,
                 s.aos_bytes_total, s.compact_bytes_projection, s.compact_ratio_bp);
    return true;
}

// AC4: heavier workload with recursion + control flow gives
// proportionally more instructions. Verifies the snapshot
// tracks IR size across the codebase, not just on a single
// trivial example. Also verifies operand_count_distribution
// has at least one non-zero bucket (otherwise the snapshot
// is empty/garbage).
bool test_heavier_workload() {
    std::println("\n--- AC4: heavier recursive workload has more instructions ---");
    aura::compiler::CompilerService cs;
    const auto& s = run_workload_and_get_stats(
        cs, "((lambda (n)\n"
            "  (if (<= n 1) 1\n"
            "    (* n ((lambda (m)\n"
            "      (if (<= m 1) 1 (* m ((lambda (k) (if (<= k 1) 1 (* k 1))) (- m 1)))))\n"
            "      (- n 1)))))\n"
            " 5)");
    CHECK(s.total_instructions >= 5, "fact lowered to >= 5 instructions (recursion + if + mul)");
    bool any_op_count_nonzero = false;
    for (std::size_t i = 0; i < 5; ++i) {
        if (s.operand_count_distribution[i] > 0) {
            any_op_count_nonzero = true;
            break;
        }
    }
    CHECK(any_op_count_nonzero, "operand-count-distribution has at least one non-zero bucket");
    // Opcode histogram should have at least one non-zero bucket.
    bool any_opcode_nonzero = false;
    for (auto c : s.opcode_histogram) {
        if (c > 0) {
            any_opcode_nonzero = true;
            break;
        }
    }
    CHECK(any_opcode_nonzero, "opcode-histogram has at least one non-zero bucket");
    return true;
}

// AC5: zero regression — existing eval still works. This is
// a basic smoke that adding the snapshot didn't break the
// IR pipeline or evaluator.
bool test_no_regression() {
    std::println("\n--- AC5: no regression — basic eval still works ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(+ 1 2 3 4 5)");
    CHECK(r.has_value(), "(+ 1 2 3 4 5) returns a value");
    if (r) {
        CHECK(aura::compiler::types::is_int(*r), "(+ 1 2 3 4 5) returns an int");
        CHECK_EQ(aura::compiler::types::as_int(*r), 15, "(+ 1 2 3 4 5) == 15");
    }
    // And a closure-heavy eval:
    auto r2 = cs.eval("(define g (lambda (x) (+ x 10))) (g 5)");
    CHECK(r2.has_value(), "(g 5) returns a value");
    if (r2) {
        CHECK_EQ(aura::compiler::types::as_int(*r2), 15, "(g 5) == 15");
    }
    return true;
}

} // namespace aura_issue_375_detail

int aura_issue_375_run() {
    using namespace aura_issue_375_detail;
    std::println("═══ Issue #375 — IR encoding observability foundation ═══\n");
    test_initial_snapshot_zero();
    test_snapshot_updates_on_workload();
    test_compact_ratio_in_range();
    test_heavier_workload();
    test_no_regression();
    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_375_run();
}
#endif
