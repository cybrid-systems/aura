// test_issue_143.cpp — Verify Issue #143 partial deliverable
// (escape analysis integration into the existing pass infra).
//
// #143 was a roadmap issue proposing 3 workstreams totaling
// 8-13 weeks:
//   1. Fine-grained incremental compilation v3 (2-3 weeks)
//   2. LLVM JIT backend completion (4-6 weeks)
//   3. High-impact IR opt passes (2-4 weeks)
//
// The only piece that fits in a verify+close cycle is escape
// analysis integration: an `EscapeAnalysisWrap` struct already
// existed in `service.ixx` (it consumed the aura::jit escape
// analyzer), but it was never tested end-to-end and was not
// part of the documented pass_manager surface. This PR:
//   1. Exported the existing struct (no implementation change)
//   2. Added tests/test_issue_143.cpp to verify the escape
//      analysis pipeline produces correct maps on hand-crafted
//      IRFunctions.
//
// Tests:
//   AC #1: Wrap is a valid pass (run on empty module)
//   AC #2: Return propagates escape to its operand
//   AC #3: Call propagates escape to all arg slots
//   AC #4: MakePair propagates escape to its car/cdr
//   AC #5: Capture propagates escape to its source
//   AC #6: Pure computation that does NOT escape (negative case)
//   AC #7: Per-function maps tracked separately
//   AC #8: Multi-iteration fixpoint (escape propagates through chain)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <print>
#include <string>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.pass_manager;



using aura::ir::IROpcode;
using aura::ir::IRFunction;
using aura::ir::IRModule;
using aura::ir::BasicBlock;
using aura::ir::IRInstruction;

static IRFunction make_fn_with_block(std::uint32_t local_count = 8) {
    IRFunction fn;
    fn.name = "test_fn";
    fn.local_count = local_count;
    fn.arg_count = 0;
    fn.entry_block = 0;
    BasicBlock bb;
    bb.id = 0;
    fn.blocks.push_back(std::move(bb));
    return fn;
}

// Helper: add an instruction to the entry block
static void add_instr(IRFunction& fn, IROpcode op, std::uint32_t a = 0,
                       std::uint32_t b = 0, std::uint32_t c = 0, std::uint32_t d = 0) {
    IRInstruction instr;
    instr.opcode = op;
    instr.operands = {a, b, c, d};
    fn.blocks[0].instructions.push_back(instr);
}

// Read a slot's escape status from a function's map. Returns
// -1 if the map is unavailable.
static int escape_at(const std::vector<std::uint8_t>& map, std::uint32_t slot) {
    if (slot >= map.size()) return -1;
    return map[slot] != 0 ? 1 : 0;
}

// ═══════════════════════════════════════════════════════════════
// AC #1: EscapeAnalysisWrap integrates as a pass
// ═══════════════════════════════════════════════════════════════

bool test_wrap_basic() {
    std::println("\n--- Test 1.1: EscapeAnalysisWrap runs on empty module ---");
    IRModule mod;
    mod.add_function(make_fn_with_block());

    aura::compiler::EscapeAnalysisWrap pass;
    pass.run(mod);
    CHECK(!pass.has_error(), "EscapeAnalysisWrap reports no error on empty module");
    CHECK(pass.all_maps().size() == 1, "all_maps().size() matches module.functions.size()");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #2: Return propagates escape to its operand
// ═══════════════════════════════════════════════════════════════

bool test_return_propagates() {
    std::println("\n--- Test 2.1: Return makes its operand escape ---");
    IRModule mod;
    mod.add_function(make_fn_with_block());
    add_instr(mod.functions[0], IROpcode::Return, /*value_slot=*/3);

    aura::compiler::EscapeAnalysisWrap pass;
    pass.run(mod);
    auto& map = pass.all_maps()[0];
    CHECK(escape_at(map, 3) == 1, "slot 3 (the Return value) is ESCAPED");
    CHECK(escape_at(map, 0) == 0, "slot 0 is NOT escaped");
    CHECK(escape_at(map, 1) == 0, "slot 1 is NOT escaped");
    CHECK(escape_at(map, 7) == 0, "slot 7 is NOT escaped");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #3: Call propagates escape to all arg slots
// ═══════════════════════════════════════════════════════════════

bool test_call_propagates() {
    std::println("\n--- Test 3.1: Call makes arg slots escape ---");
    IRModule mod;
    mod.add_function(make_fn_with_block());
    // (call result=4 callee=1 arg_begin=2 arg_count=1)
    add_instr(mod.functions[0], IROpcode::Call, /*result=*/4, /*callee=*/1,
              /*arg_begin=*/2, /*arg_count=*/1);

    aura::compiler::EscapeAnalysisWrap pass;
    pass.run(mod);
    auto& map = pass.all_maps()[0];
    // Call arg slots should escape.
    CHECK(escape_at(map, 1) == 1, "callee slot 1 escapes (Call arg)");
    CHECK(escape_at(map, 2) == 1, "arg slot 2 escapes (Call arg)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #4: MakePair propagates escape to its car/cdr (via result)
// ═══════════════════════════════════════════════════════════════

bool test_makepair_propagates() {
    std::println("\n--- Test 4.1: MakePair result→car/cdr propagation ---");
    // The existing analyzer only propagates BACK from an escaped
    // result. So we need: MakePair(5, 1, 2) then Return(5) to
    // exercise the propagation. Then car (1) and cdr (2) should
    // be marked escaped via the result-chain.
    IRModule mod;
    mod.add_function(make_fn_with_block());
    add_instr(mod.functions[0], IROpcode::MakePair, /*result=*/5, /*car=*/1, /*cdr=*/2);
    add_instr(mod.functions[0], IROpcode::Return, /*value_slot=*/5);

    aura::compiler::EscapeAnalysisWrap pass;
    pass.run(mod);
    auto& map = pass.all_maps()[0];
    CHECK(escape_at(map, 5) == 1, "MakePair result 5 escapes (Return)");
    CHECK(escape_at(map, 1) == 1, "car slot 1 escapes (via result chain)");
    CHECK(escape_at(map, 2) == 1, "cdr slot 2 escapes (via result chain)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #5: Capture propagates escape to its source
// ═══════════════════════════════════════════════════════════════

bool test_capture_propagates() {
    std::println("\n--- Test 5.1: Capture makes the captured var escape ---");
    // (capture closure=3 env_idx=0 var=2) — the existing analyzer
    // marks ops[2] (the var) as escaped. The closure slot itself
    // is not marked — captures go INTO the closure, not out of it.
    IRModule mod;
    mod.add_function(make_fn_with_block());
    add_instr(mod.functions[0], IROpcode::Capture, /*closure=*/3, /*env_idx=*/0, /*var=*/2);

    aura::compiler::EscapeAnalysisWrap pass;
    pass.run(mod);
    auto& map = pass.all_maps()[0];
    CHECK(escape_at(map, 2) == 1, "captured var slot 2 escapes (into closure env)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #6: Pure computation that does NOT escape
// ═══════════════════════════════════════════════════════════════

bool test_no_escape() {
    std::println("\n--- Test 6.1: pure computation does not escape ---");
    IRModule mod;
    mod.add_function(make_fn_with_block());
    // (add result=0 a=1 b=2) — no Return / Call / MakePair.
    add_instr(mod.functions[0], IROpcode::Add, /*result=*/0, /*a=*/1, /*b=*/2);

    aura::compiler::EscapeAnalysisWrap pass;
    pass.run(mod);
    auto& map = pass.all_maps()[0];
    CHECK(escape_at(map, 0) == 0, "Add result slot 0 is NOT escaped");
    CHECK(escape_at(map, 1) == 0, "Add operand a (slot 1) is NOT escaped");
    CHECK(escape_at(map, 2) == 0, "Add operand b (slot 2) is NOT escaped");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #7: Per-function maps tracked separately
// ═══════════════════════════════════════════════════════════════

bool test_per_function_maps() {
    std::println("\n--- Test 7.1: per-function maps tracked separately ---");
    IRModule mod;
    mod.add_function(make_fn_with_block());
    mod.add_function(make_fn_with_block());
    // fn0: Add then Return — slot 0 escapes via Return
    add_instr(mod.functions[0], IROpcode::Add, 0, 1, 2);
    add_instr(mod.functions[0], IROpcode::Return, 0);
    // fn1: just Mul — no escape points
    add_instr(mod.functions[1], IROpcode::Mul, 3, 4, 5);

    aura::compiler::EscapeAnalysisWrap pass;
    pass.run(mod);
    CHECK(pass.all_maps().size() == 2, "two functions → two maps");

    auto& m0 = pass.all_maps()[0];
    auto& m1 = pass.all_maps()[1];
    CHECK(escape_at(m0, 0) == 1, "fn0 result 0 escapes (Return)");
    CHECK(escape_at(m1, 3) == 0, "fn1 result 3 is NOT escaped (no Return)");
    CHECK(escape_at(m1, 4) == 0, "fn1 operand 4 is NOT escaped");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #8: Multi-iteration fixpoint (escape propagates through chain)
// ═══════════════════════════════════════════════════════════════

bool test_fixpoint_propagation() {
    std::println("\n--- Test 8.1: escape propagates through Local chain ---");
    // The existing analyzer propagates through Local (one level)
    // and MakePair (one level). It does NOT propagate through Add
    // (binary arithmetic). So:
    //   Local(0, 1)        — slot 0 is a copy of slot 1
    //   Return(0)           — slot 0 escapes
    //   Backward: slot 1 should also escape (via Local propagation)
    IRModule mod;
    mod.add_function(make_fn_with_block());
    add_instr(mod.functions[0], IROpcode::Local, /*result=*/0, /*src=*/1);
    add_instr(mod.functions[0], IROpcode::Return, /*value_slot=*/0);

    aura::compiler::EscapeAnalysisWrap pass;
    pass.run(mod);
    auto& map = pass.all_maps()[0];
    CHECK(escape_at(map, 0) == 1, "Local result 0 escapes (Return)");
    CHECK(escape_at(map, 1) == 1, "Local source 1 escapes (backward propagation)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════

int run_issue_143() {
    std::println("═══ Issue #143 verification tests (escape analysis) ═══\n");

    std::println("── AC #1: Wrap integrates as a pass ──");
    test_wrap_basic();

    std::println("\n── AC #2: Return propagates escape ──");
    test_return_propagates();

    std::println("\n── AC #3: Call propagates escape ──");
    test_call_propagates();

    std::println("\n── AC #4: MakePair propagates escape ──");
    test_makepair_propagates();

    std::println("\n── AC #5: Capture propagates escape ──");
    test_capture_propagates();

    std::println("\n── AC #6: Pure computation does not escape ──");
    test_no_escape();

    std::println("\n── AC #7: Per-function maps ──");
    test_per_function_maps();

    std::println("\n── AC #8: Multi-iteration fixpoint ──");
    test_fixpoint_propagation();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
