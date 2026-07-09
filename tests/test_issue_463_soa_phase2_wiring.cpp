// @category: unit
// @reason: pure C++ test of SoA Phase 2 scaffolding
//          (IRFunctionSoA + IRInstructionView + IRModuleV2 +
//          to_aos_view + SoAtoAoSBridgePass) + EDSL
//          (query:soa-adoption-stats) primitive

// test_issue_463_soa_phase2_wiring.cpp — Issue #463:
// SoA Phase 2 wiring (refine #429).
//
// Full scope is multi-week (lower + eval + at least 3
// Passes consume SoA views without fallback; per-block
// instruction-level dirty propagation; selective
// recompile of affected blocks; perf benchmark).
//
// Scope-limited close ships the SCAFFOLDING + COUNTER
// LAYER (precondition for the rest):
//   1. IRFunctionSoA gains `name` + `local_count` fields
//      (mirrors AoS IRFunction, needed for SoA→AoS view)
//   2. IRModuleV2::add_function (mirrors AoS pattern)
//   3. to_aos_view(const IRFunctionSoA&) free function
//      (O(n) conversion, used by the bridge pass)
//   4. SoAtoAoSBridgePass<P> template — wraps an existing
//      AoS Pass, runs it on the SoA→AoS view, bumps
//      counters
//   5. 3 CompilerMetrics atomics: soa_functions_visited,
//      soa_instructions_visited, aos_view_built_count
//   6. (query:soa-adoption-stats) Aura primitive — 3-field
//      hash
//   7. (stats:count) 49 → 50
//
// Test cases:
//   AC1:  IRFunctionSoA has name + local_count fields
//   AC2:  IRModuleV2::add_function returns correct index
//   AC3:  IRModuleV2::add_instruction appends to all columns
//   AC4:  IRInstructionView accessors read from columns
//   AC5:  to_aos_view(IRFunctionSoA) round-trips correctly
//   AC6:  SoAtoAoSBridgePass with empty AoS pass counters
//   AC7:  SoAtoAoSBridgePass with non-empty SoA module
//         bumps all 3 counters
//   AC8:  EDSL: (query:soa-adoption-stats) returns a hash
//         with 3 fields
//   AC9:  EDSL: (stats:count) >= 50
//   AC10: EDSL: (stats:list) includes query:soa-adoption-stats

#include "test_harness.hpp"

import std;
import aura.core;
import aura.core.ast;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.ir_soa;
import aura.compiler.pass_manager;
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_463_detail {

using aura::test::g_failed;
using aura::test::g_passed;

// ── AC1: IRFunctionSoA has name + local_count fields ──────────
bool test_ir_function_soa_fields() {
    std::println("\n--- AC1: IRFunctionSoA has name + local_count ---");
    aura::compiler::IRFunctionSoA f;
    f.name = "test";
    f.local_count = 4;
    CHECK(f.name == "test", "name field settable");
    CHECK(f.local_count == 4, "local_count field settable");
    return true;
}

// ── AC2: IRModuleV2::add_function returns correct index ───────
bool test_ir_module_v2_add_function() {
    std::println("\n--- AC2: IRModuleV2::add_function ---");
    aura::compiler::IRModuleV2 m;
    auto i0 = m.add_function("fn0", 2);
    auto i1 = m.add_function("fn1", 4);
    CHECK(i0 == 0, std::format("first add_function index == 0 (got {})", i0));
    CHECK(i1 == 1, std::format("second add_function index == 1 (got {})", i1));
    CHECK(m.functions.size() == 2, "2 functions in module");
    CHECK(m.functions[0].name == "fn0", "fn0 name preserved");
    CHECK(m.functions[1].local_count == 4, "fn1 local_count preserved");
    return true;
}

// ── AC3: IRModuleV2::add_instruction appends to all columns ──
bool test_ir_module_v2_add_instruction() {
    std::println("\n--- AC3: IRModuleV2::add_instruction ---");
    aura::compiler::IRModuleV2 m;
    m.add_function("f", 2);
    m.add_instruction(0, aura::ir::IROpcode::ConstI64, {0u, 42u, 0u, 0u},
                      /*source_node_id*/ 0, /*type_id*/ 0, /*shape_id*/ 0,
                      /*linear_state*/ 1, /*adt_variant_id*/ 0,
                      /*narrow_evidence*/ 0);
    m.add_instruction(0, aura::ir::IROpcode::Return, {0u, 0u, 0u, 0u});
    auto& fn = m.functions[0];
    CHECK(fn.opcodes_.size() == 2, "2 opcodes");
    CHECK(fn.operand0_.size() == 2, "2 operand0 entries");
    CHECK(fn.operand1_.size() == 2, "2 operand1 entries");
    CHECK(fn.linear_ownership_states_.size() == 2, "2 linear_ownership entries");
    CHECK(fn.linear_ownership_states_[0] == 1, "linear_ownership[0] == 1 (Owned)");
    CHECK(fn.opcodes_[0] == aura::ir::IROpcode::ConstI64, "first instr is ConstI64");
    CHECK(fn.opcodes_[1] == aura::ir::IROpcode::Return, "second instr is Return");
    return true;
}

// ── AC4: IRInstructionView accessors read from columns ────────
bool test_ir_instruction_view() {
    std::println("\n--- AC4: IRInstructionView accessors ---");
    aura::compiler::IRModuleV2 m;
    m.add_function("f", 2);
    m.add_instruction(0, aura::ir::IROpcode::Add, {0u, 1u, 2u, 0u}, 0, 0, 0, 0, 0, 0);
    auto v = m.view_at(0, 0);
    CHECK(v.opcode() == aura::ir::IROpcode::Add, "view opcode");
    CHECK(v.operand(0) == 0, "view operand 0");
    CHECK(v.operand(1) == 1, "view operand 1");
    CHECK(v.operand(2) == 2, "view operand 2");
    return true;
}

// ── AC5: to_aos_view round-trips correctly ────────────────────
bool test_to_aos_view_roundtrip() {
    std::println("\n--- AC5: to_aos_view round-trip ---");
    aura::compiler::IRModuleV2 m;
    m.add_function("roundtrip", 2);
    auto bid = m.add_block(0);
    m.add_instruction(0, aura::ir::IROpcode::ConstI64, {0u, 100u, 0u, 0u}, 0, 0, 0, 1, 0, 0);
    m.add_instruction(0, aura::ir::IROpcode::Return, {0u, 0u, 0u, 0u});
    m.seal_block(0, bid);

    auto aos = aura::compiler::to_aos_view(m.functions[0]);
    CHECK(aos.name == "roundtrip", "round-trip preserves name");
    CHECK(aos.local_count == 2, "round-trip preserves local_count");
    CHECK(aos.blocks.size() == 1, "round-trip preserves block count");
    CHECK(aos.blocks[0].instructions.size() == 2, "round-trip preserves instruction count");
    CHECK(aos.blocks[0].instructions[0].opcode == aura::ir::IROpcode::ConstI64,
          "round-trip preserves opcode");
    CHECK(aos.blocks[0].instructions[0].operands[1] == 100, "round-trip preserves operand1");
    CHECK(aos.blocks[0].instructions[0].linear_ownership_state == 1,
          "round-trip preserves linear_ownership_state");
    return true;
}

// ── AC6: SoAtoAoSBridgePass with empty AoS pass counters ──────
bool test_bridge_empty() {
    std::println("\n--- AC6: SoAtoAoSBridgePass empty ---");
    aura::compiler::IRModuleV2 m;
    // No functions added — should bump 0 counters.
    aura::compiler::ComputeKindWrap ck;
    aura::compiler::SoAtoAoSBridgePass bridge(ck);
    bool ok = bridge.run(m);
    CHECK(ok, "empty bridge run() returns true");
    CHECK(bridge.soa_functions_visited() == 0, "0 functions visited");
    CHECK(bridge.soa_instructions_visited() == 0, "0 instructions visited");
    CHECK(bridge.aos_view_built_count() == 0, "0 AoS views built");
    return true;
}

// ── AC7: SoAtoAoSBridgePass with non-empty SoA module ─────────
bool test_bridge_nonempty() {
    std::println("\n--- AC7: SoAtoAoSBridgePass non-empty ---");
    aura::compiler::IRModuleV2 m;
    m.add_function("f0", 2);
    auto b0 = m.add_block(0);
    m.add_instruction(0, aura::ir::IROpcode::ConstI64, {0u, 1u, 0u, 0u});
    m.add_instruction(0, aura::ir::IROpcode::Return, {0u, 0u, 0u, 0u});
    m.seal_block(0, b0);
    m.add_function("f1", 3);
    auto b1 = m.add_block(1);
    m.add_instruction(1, aura::ir::IROpcode::ConstI64, {0u, 2u, 0u, 0u});
    m.add_instruction(1, aura::ir::IROpcode::ConstI64, {0u, 3u, 0u, 0u});
    m.add_instruction(1, aura::ir::IROpcode::Add, {0u, 0u, 1u, 0u});
    m.add_instruction(1, aura::ir::IROpcode::Return, {0u, 0u, 0u, 0u});
    m.seal_block(1, b1);

    aura::compiler::ComputeKindWrap ck;
    aura::compiler::SoAtoAoSBridgePass bridge(ck);
    bool ok = bridge.run(m);
    CHECK(ok, "non-empty bridge run() returns true");
    CHECK(bridge.soa_functions_visited() == 2, "2 functions visited");
    CHECK(bridge.soa_instructions_visited() == 6, "6 instructions visited (2 + 4)");
    CHECK(bridge.aos_view_built_count() == 2, "2 AoS views built");
    CHECK(bridge.aos_view().functions.size() == 2, "AoS view has 2 functions");
    return true;
}

// ── AC8: EDSL: (query:soa-adoption-stats) returns a hash ──────
bool test_edsl_stats_returns_hash() {
    std::println("\n--- AC8: EDSL (query:soa-adoption-stats) ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:soa-adoption-stats)");
    if (!r) {
        CHECK(false, "eval returned error");
        return true;
    }
    auto v = *r;
    CHECK(aura::compiler::types::is_hash(v), "(query:soa-adoption-stats) returns a hash");
    for (auto key : {"soa-functions-visited", "soa-instructions-visited", "aos-view-built-count"}) {
        auto rr = cs.eval(std::format("(hash-ref (query:soa-adoption-stats) '{}')", key));
        if (!rr) {
            CHECK(false, std::format("hash-ref for '{}' failed", key));
            continue;
        }
        CHECK(aura::compiler::types::is_int(*rr), std::format("hash-ref '{}' returns int", key));
    }
    return true;
}

// ── AC9: (stats:count) >= 50
bool test_stats_count() {
    std::println("\n--- AC9: (stats:count) >= 50 ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(stats:count)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        CHECK(false, "stats:count not int");
        return true;
    }
    auto n = aura::compiler::types::as_int(*r);
    CHECK(n >= 50, std::format("stats:count >= 50 (got {})", n));
    return true;
}

// ── AC10: (stats:list) includes query:soa-adoption-stats
bool test_stats_list_includes() {
    std::println("\n--- AC10: (stats:list) includes query:soa-adoption-stats ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval(R"((if (member "query:soa-adoption-stats" (stats:list)) #t #f))");
    if (!r) {
        CHECK(false, "eval failed");
        return true;
    }
    auto v = *r;
    CHECK(v.val != 0 && !aura::compiler::types::is_void(v),
          "query:soa-adoption-stats is in (stats:list)");
    return true;
}

} // namespace aura_issue_463_detail

int aura_issue_463_soa_phase2_wiring_run() {
    using namespace aura_issue_463_detail;
    std::println("═══ Issue #463 SoA Phase 2 wiring tests ═══");

    test_ir_function_soa_fields();
    test_ir_module_v2_add_function();
    test_ir_module_v2_add_instruction();
    test_ir_instruction_view();
    test_to_aos_view_roundtrip();
    test_bridge_empty();
    test_bridge_nonempty();
    test_edsl_stats_returns_hash();
    test_stats_count();
    test_stats_list_includes();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_463_soa_phase2_wiring_run();
}
#endif
