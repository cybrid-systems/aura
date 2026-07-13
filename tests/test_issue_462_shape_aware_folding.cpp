// @category: unit
// @reason: pure C++ test of ShapeAwareFoldingPass + EDSL
//          (query:shape-folding-stats) primitive + pipeline
//          wiring

// test_issue_462_shape_aware_folding.cpp — Issue #462:
// New ShapeAwareFoldingPass + Linear elision synergy using
// GuardShape / narrow_evidence / linear_ownership_state.
//
// Full scope is multi-week (per-shape-id unchecked OpAdd
// specialization, narrow-evidence rewrite, ADT-variant
// specialization, integration with GuardShape deopt).
//
// Scope-limited close ships:
//   1. ShapeAwareFoldingPass scaffolding in pass_manager.ixx
//      (run() walks each function's blocks, drives linear
//      elision using the escape_map from EscapeAnalysis,
//      counts narrow-evidence type-checks without rewriting)
//   2. 4 CompilerMetrics atomics: shape_fold_count,
//      shape_linear_elide_count, shape_narrow_check_count,
//      guard_shape_hits
//   3. Pass wired into service.ixx pipeline (after
//      EscapeAnalysis, before JIT)
//   4. (query:shape-folding-stats) Aura primitive — 4-field
//      hash
//   5. (stats:count) 48 → 49
//   6. Docs regen
//
// Test cases:
//   AC1:  ShapeAwareFoldingPass has the expected name()
//   AC2:  Empty module — all counters 0
//   AC3:  IRFunction with a non-escaping Owned slot +
//        MoveOp on it → linear elide happens
//   AC4:  IRFunction with an escaping Owned slot +
//        MoveOp on it → MoveOp preserved (no elision)
//   AC5:  IRFunction with a GuardShape → guard_shape_hits
//        counter bumps
//   AC6:  IRFunction with a CastOp on a slot with
//        narrow_evidence != 0 → narrow_check_count bumps
//   AC7:  EDSL: (query:shape-folding-stats) returns a hash
//        with 4 fields
//   AC8:  EDSL: (stats:count) >= 49
//   AC9:  EDSL: (stats:list) includes query:shape-folding-stats
//   AC10: Pass satisfies the Pass concept (compile-time check)

#include "test_harness.hpp"

import std;
import aura.core;
import aura.core.ast;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_462_detail {

using aura::test::g_failed;
using aura::test::g_passed;

// ── AC1: pass has the expected name() ─────────────────────────
bool test_pass_name() {
    std::println("\n--- AC1: ShapeAwareFoldingPass name() ---");
    aura::compiler::ShapeAwareFoldingPass saf;
    auto n = std::string(saf.name());
    CHECK(n == "shape-aware-folding", std::format("name == 'shape-aware-folding' (got '{}')", n));
    return true;
}

// ── AC2: empty module — all counters 0 ────────────────────────
bool test_empty_module() {
    std::println("\n--- AC2: empty module ---");
    aura::ir::IRModule mod;
    aura::compiler::ShapeAwareFoldingPass saf;
    saf.run(mod);
    CHECK(saf.fold_count() == 0, "fold_count == 0");
    CHECK(saf.linear_elide_count() == 0, "linear_elide_count == 0");
    CHECK(saf.narrow_check_count() == 0, "narrow_check_count == 0");
    CHECK(saf.guard_shape_hits() == 0, "guard_shape_hits == 0");
    return true;
}

// ── AC3: non-escaping Owned slot → MoveOp elided ──────────────
//
// IRFunction with 4 slots:
//   slot 0: result of ConstI64 (linear_ownership_state = 1 = Owned)
//   slot 1: MoveOp(slot 0 → slot 1)  (this is the elide target)
//   slot 2: result of ConstI64 (also Owned, but escape map marks it)
//   slot 3: MoveOp(slot 2 → slot 3)
//
// Escape map: slot 0 = 0 (not escaping), slot 2 = 1 (escaping).
// After run: slot 1's MoveOp → Nop, slot 3's MoveOp preserved.
bool test_linear_elide_non_escaping() {
    std::println("\n--- AC3: linear elide on non-escaping slot ---");
    aura::ir::IRModule mod;
    mod.entry_function_id = 0;
    aura::ir::IRFunction func;
    func.id = 0;
    func.name = "test_elide";
    func.local_count = 4;
    func.blocks.push_back({0});

    // slot 0 = ConstI64 with linear_ownership_state = 1 (Owned)
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::ConstI64,
        .operands = {0u, 42u, 0u, 0u},
        .linear_ownership_state = 1,
    });
    // slot 1 = MoveOp(slot 0 → slot 1)
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::MoveOp,
        .operands = {1u, 0u, 0u, 0u},
    });
    // slot 2 = ConstI64 with linear_ownership_state = 1 (Owned)
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::ConstI64,
        .operands = {2u, 99u, 0u, 0u},
        .linear_ownership_state = 1,
    });
    // slot 3 = MoveOp(slot 2 → slot 3)
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::MoveOp,
        .operands = {3u, 2u, 0u, 0u},
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Return,
        .operands = {0u, 0u, 0u, 0u},
    });
    mod.functions.push_back(func);

    aura::compiler::ShapeAwareFoldingPass saf;
    // escape_map: [0]=0 (non-escaping), [1]=0, [2]=1 (escaping), [3]=1
    saf.set_escape_map("test_elide", {0, 0, 1, 1});
    saf.run(mod);
    CHECK(saf.linear_elide_count() == 1,
          std::format("linear_elide_count == 1 (got {})", saf.linear_elide_count()));
    CHECK(saf.fold_count() == 1, std::format("fold_count == 1 (got {})", saf.fold_count()));
    // The MoveOp at slot 1 should be Nop'd; the MoveOp at slot 3 preserved.
    auto& blk = mod.functions[0].blocks[0].instructions;
    bool slot1_is_nop = (blk[1].opcode == aura::ir::IROpcode::Nop);
    bool slot3_is_moveop = (blk[3].opcode == aura::ir::IROpcode::MoveOp);
    CHECK(slot1_is_nop, "slot 1's MoveOp is now Nop");
    CHECK(slot3_is_moveop, "slot 3's MoveOp preserved (escape map says escaping)");
    return true;
}

// ── AC4: escaping slot → MoveOp preserved ─────────────────────
//
// Same IR as AC3 but escape_map: [0]=1, [2]=1 (all escaping).
// No elision should happen.
bool test_linear_no_elide_escaping() {
    std::println("\n--- AC4: no elide on escaping slot ---");
    aura::ir::IRModule mod;
    mod.entry_function_id = 0;
    aura::ir::IRFunction func;
    func.id = 0;
    func.name = "test_escape";
    func.local_count = 2;
    func.blocks.push_back({0});
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::ConstI64,
        .operands = {0u, 1u, 0u, 0u},
        .linear_ownership_state = 1,
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::MoveOp,
        .operands = {1u, 0u, 0u, 0u},
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Return,
        .operands = {0u, 0u, 0u, 0u},
    });
    mod.functions.push_back(func);

    aura::compiler::ShapeAwareFoldingPass saf;
    saf.set_escape_map("test_escape", {1, 1}); // slot 0 is escaping
    saf.run(mod);
    CHECK(saf.linear_elide_count() == 0, "linear_elide_count == 0 (all escaping)");
    CHECK(saf.fold_count() == 0, "fold_count == 0");
    auto& blk = mod.functions[0].blocks[0].instructions;
    CHECK(blk[1].opcode == aura::ir::IROpcode::MoveOp, "MoveOp preserved on escaping slot");
    return true;
}

// ── AC5: GuardShape → guard_shape_hits bumps ─────────────────
bool test_guard_shape_hits() {
    std::println("\n--- AC5: GuardShape hit counting ---");
    aura::ir::IRModule mod;
    mod.entry_function_id = 0;
    aura::ir::IRFunction func;
    func.id = 0;
    func.name = "test_gs";
    func.local_count = 1;
    func.blocks.push_back({0});
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::GuardShape,
        .operands = {0u, 0u, 1u, 0u}, // result=0, arg=0, expected_shape=1, generic=0
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Return,
        .operands = {0u, 0u, 0u, 0u},
    });
    mod.functions.push_back(func);

    aura::compiler::ShapeAwareFoldingPass saf;
    saf.run(mod);
    CHECK(saf.guard_shape_hits() == 1,
          std::format("guard_shape_hits == 1 (got {})", saf.guard_shape_hits()));
    return true;
}

// ── AC6: CastOp with narrow_evidence → narrow_check_count bumps
bool test_narrow_evidence_count() {
    std::println("\n--- AC6: narrow-evidence check count ---");
    aura::ir::IRModule mod;
    mod.entry_function_id = 0;
    aura::ir::IRFunction func;
    func.id = 0;
    func.name = "test_narrow";
    func.local_count = 2;
    func.blocks.push_back({0});
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Local,
        .operands = {0u, 0u, 0u, 0u},
    });
    // CastOp with narrow_evidence = 1 (number? predicate applied)
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::CastOp,
        .operands = {1u, 0u, 0u, 0u}, // result=1, value=0, type_tag=0
        .narrow_evidence = 1,
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Return,
        .operands = {0u, 0u, 0u, 0u},
    });
    mod.functions.push_back(func);

    aura::compiler::ShapeAwareFoldingPass saf;
    saf.run(mod);
    CHECK(saf.narrow_check_count() == 1,
          std::format("narrow_check_count == 1 (got {})", saf.narrow_check_count()));
    return true;
}

// ── AC7: EDSL: (query:shape-folding-stats) returns a hash
//        with 4 fields
bool test_edsl_stats_returns_hash() {
    std::println("\n--- AC7: EDSL (query:shape-folding-stats) ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:shape-folding-stats)");
    if (!r) {
        CHECK(false, "eval returned error");
        return true;
    }
    auto v = *r;
    CHECK(aura::compiler::types::is_hash(v), "(query:shape-folding-stats) returns a hash");
    // Check each of the 4 fields exists
    for (auto key : {"shape-fold-count", "shape-linear-elide-count", "shape-narrow-check-count",
                     "guard-shape-hits"}) {
        auto rr = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:shape-folding-stats\") '{}')", key));
        if (!rr) {
            CHECK(false, std::format("hash-ref for '{}' failed", key));
            continue;
        }
        CHECK(aura::compiler::types::is_int(*rr), std::format("hash-ref '{}' returns int", key));
    }
    return true;
}

// ── AC8: (stats:count) >= 49
bool test_stats_count() {
    std::println("\n--- AC8: (stats:count) >= 49 ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(stats:count)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        CHECK(false, "stats:count not int");
        return true;
    }
    auto n = aura::compiler::types::as_int(*r);
    CHECK(n >= 49, std::format("stats:count >= 49 (got {})", n));
    return true;
}

// ── AC9: (stats:list) includes query:shape-folding-stats
bool test_stats_list_includes() {
    std::println("\n--- AC9: (stats:list) includes query:shape-folding-stats ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval(R"((if (member "query:shape-folding-stats" (stats:list)) #t #f))");
    if (!r) {
        CHECK(false, "eval failed");
        return true;
    }
    auto v = *r;
    CHECK(v.val != 0 && !aura::compiler::types::is_void(v),
          "query:shape-folding-stats is in (stats:list)");
    return true;
}

// ── AC10: pass satisfies Pass concept (compile-time check)
//
// Verified by the import + the use of `run_pipeline` below.
bool test_pass_concept_satisfied() {
    std::println("\n--- AC10: Pass concept satisfaction ---");
    CHECK(static_cast<bool>(aura::compiler::Pass<aura::compiler::ShapeAwareFoldingPass>),
          "ShapeAwareFoldingPass satisfies Pass concept");
    return true;
}

} // namespace aura_issue_462_detail

int aura_issue_462_shape_aware_folding_run() {
    using namespace aura_issue_462_detail;
    std::println("═══ Issue #462 ShapeAwareFoldingPass tests ═══");

    test_pass_name();
    test_empty_module();
    test_linear_elide_non_escaping();
    test_linear_no_elide_escaping();
    test_guard_shape_hits();
    test_narrow_evidence_count();
    test_edsl_stats_returns_hash();
    test_stats_count();
    test_stats_list_includes();
    test_pass_concept_satisfied();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_462_shape_aware_folding_run();
}
#endif
