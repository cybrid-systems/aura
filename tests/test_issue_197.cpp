// test_issue_197.cpp — Issue #197: branch-aware inliner + parameter
// renaming (extends InlinePass from #171).
//
// P1 performance issue. The pre-#197 inliner only handles
// single-block callees that return a constant. The branch-aware
// inliner accepts multi-block callees with Branch+Return and any
// number of parameters.
//
// This PR ships:
//   1. is_inlinable_branch_aware predicate (handles multi-block
//      Branch+Return, with parameters, no loops, no nested
//      calls, no observable side effects)
//   2. try_inline_branch_aware transformation (single-block
//      callee with parameters: insert Local copies for params,
//      copy callee body, remap slot ids)
//   3. inlined_branch_aware_count() accessor (separate counter
//      from inlined_count_ which tracks the pre-#197 path)
//
// Test strategy: direct C++ tests on IRModule + IRFunction, no
// Aura layer needed (matches test_issue_171.cpp pattern).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import std;
import aura.core;
import aura.compiler.ir;
import aura.compiler.pass_manager;



#define PRINTLN(msg) do { std::fprintf(stdout, "%s\n", (msg)); } while(0)

// Helper: build a single-block callee with N params and a body
// that does Local(2, 0) + Local(3, 1) + Add(4, 2, 3) + Return(4).
// This is a real function: takes 2 params, returns their sum.
// Uses 5 locals (0, 1 = params, 2, 3 = locals, 4 = result).
static aura::ir::IRFunction make_add2_callee() {
    aura::ir::IRFunction func;
    func.name = "add2";
    func.local_count = 5;
    func.arg_count = 2;
    func.blocks.push_back({0});
    func.blocks[0].instructions = {
        {aura::ir::IROpcode::Local, {2, 0, 0, 0}, 0, 0},   // local2 = local0 (param 0)
        {aura::ir::IROpcode::Local, {3, 1, 0, 0}, 0, 0},   // local3 = local1 (param 1)
        {aura::ir::IROpcode::Add,   {4, 2, 3, 0}, 0, 0},   // local4 = local2 + local3
        {aura::ir::IROpcode::Return, {4, 0, 0, 0}, 0, 0},   // return local4
    };
    return func;
}

// Helper: build a multi-block callee with a Branch+Return.
// Block 0: Local, Branch(cond, 1, 2)
// Block 1: ConstI64(1), Return(1)
// Block 2: ConstI64(0), Return(2)
static aura::ir::IRFunction make_branch_aware_callee() {
    aura::ir::IRFunction func;
    func.name = "select";
    func.local_count = 2;
    func.arg_count = 1;
    func.blocks.push_back({0});
    func.blocks.push_back({1});
    func.blocks.push_back({2});
    func.blocks[0].instructions = {
        // Branch(cond=local0, true=block1, false=block2)
        {aura::ir::IROpcode::Branch, {0, 1, 2, 0}, 0, 0},
    };
    func.blocks[1].instructions = {
        {aura::ir::IROpcode::ConstI64, {1, 7, 0, 0}, 0, 1},
        {aura::ir::IROpcode::Return,   {1, 0, 0, 0}, 0, 0},
    };
    func.blocks[2].instructions = {
        {aura::ir::IROpcode::ConstI64, {2, 9, 0, 0}, 0, 1},
        {aura::ir::IROpcode::Return,   {2, 0, 0, 0}, 0, 0},
    };
    return func;
}

// ═════════════════════════════════════════════════════════════
// AC1: is_inlinable_branch_aware predicate
// ═════════════════════════════════════════════════════════════

bool test_predicate_accepts_single_block_with_params() {
    PRINTLN("\n--- Test 1.1: is_inlinable_branch_aware accepts single-block + params ---");
    auto func = make_add2_callee();
    bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(func);
    CHECK(ok, "single-block + params + Return: inlinable");
    return true;
}

bool test_predicate_accepts_multi_block_branch_return() {
    PRINTLN("\n--- Test 1.2: is_inlinable_branch_aware accepts multi-block Branch+Return ---");
    auto func = make_branch_aware_callee();
    bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(func);
    CHECK(ok, "multi-block + Branch+Return + 1 param: inlinable");
    return true;
}

bool test_predicate_rejects_nested_call() {
    PRINTLN("\n--- Test 1.3: is_inlinable_branch_aware rejects nested Call ---");
    aura::ir::IRFunction func;
    func.name = "with_call";
    func.local_count = 1;
    func.arg_count = 0;
    func.blocks.push_back({0});
    func.blocks[0].instructions = {
        {aura::ir::IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
        {aura::ir::IROpcode::Call,     {0, 0, 0, 0}, 0, 0},  // nested Call
        {aura::ir::IROpcode::Return,   {0, 0, 0, 0}, 0, 0},
    };
    bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(func);
    CHECK(!ok, "callee with nested Call: NOT inlinable");
    return true;
}

bool test_predicate_rejects_nested_make_closure() {
    PRINTLN("\n--- Test 1.4: is_inlinable_branch_aware rejects nested MakeClosure ---");
    aura::ir::IRFunction func;
    func.name = "with_closure";
    func.local_count = 1;
    func.arg_count = 0;
    func.blocks.push_back({0});
    func.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Return,     {0, 0, 0, 0}, 0, 0},
    };
    bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(func);
    CHECK(!ok, "callee with nested MakeClosure: NOT inlinable");
    return true;
}

bool test_predicate_rejects_cell_set() {
    PRINTLN("\n--- Test 1.5: is_inlinable_branch_aware rejects CellSet (side effect) ---");
    aura::ir::IRFunction func;
    func.name = "with_cell_set";
    func.local_count = 2;
    func.arg_count = 0;
    func.blocks.push_back({0});
    func.blocks[0].instructions = {
        {aura::ir::IROpcode::NewCell, {0, 0, 0, 0}, 0, 0},
        {aura::ir::IROpcode::CellSet, {0, 1, 0, 0}, 0, 0},  // side effect
        {aura::ir::IROpcode::Return,  {1, 0, 0, 0}, 0, 0},
    };
    bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(func);
    CHECK(!ok, "callee with CellSet side effect: NOT inlinable");
    return true;
}

bool test_predicate_rejects_loop() {
    PRINTLN("\n--- Test 1.6: is_inlinable_branch_aware rejects loop (back-edge) ---");
    aura::ir::IRFunction func;
    func.name = "with_loop";
    func.local_count = 2;
    func.arg_count = 0;
    // Block 0 branches to itself (self-loop)
    func.blocks.push_back({0});
    func.blocks[0].instructions = {
        {aura::ir::IROpcode::ConstI64, {0, 0, 0, 0}, 0, 1},
        {aura::ir::IROpcode::Branch,   {0, 0, 0, 0}, 0, 0},  // cond=local0, true=0, false=0
    };
    bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(func);
    CHECK(!ok, "callee with self-loop: NOT inlinable");
    return true;
}

bool test_predicate_rejects_oversize() {
    PRINTLN("\n--- Test 1.7: is_inlinable_branch_aware rejects >8 blocks ---");
    aura::ir::IRFunction func;
    func.name = "too_big";
    func.local_count = 1;
    func.arg_count = 0;
    // 9 blocks (1 entry + 8 leaf)
    for (std::uint32_t i = 0; i < 9; ++i) {
        func.blocks.push_back({i});
        func.blocks[i].instructions = {
            {aura::ir::IROpcode::Return, {0, 0, 0, 0}, 0, 0},
        };
    }
    // Add a Branch from block 0 to a leaf to make it a CFG
    func.blocks[0].instructions = {
        {aura::ir::IROpcode::Branch, {0, 1, 2, 0}, 0, 0},
    };
    bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(func);
    CHECK(!ok, "callee with >8 blocks: NOT inlinable (size heuristic)");
    return true;
}

bool test_predicate_rejects_no_terminator() {
    PRINTLN("\n--- Test 1.8: is_inlinable_branch_aware rejects block without terminator ---");
    aura::ir::IRFunction func;
    func.name = "no_terminator";
    func.local_count = 1;
    func.arg_count = 0;
    func.blocks.push_back({0});
    func.blocks[0].instructions = {
        {aura::ir::IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
        // No Branch/Jump/Return at the end
    };
    bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(func);
    CHECK(!ok, "callee with no terminator: NOT inlinable");
    return true;
}

// ═════════════════════════════════════════════════════════════
// AC2: try_inline_branch_aware transformation (single-block + params)
// ═════════════════════════════════════════════════════════════

bool test_inline_single_block_with_params() {
    PRINTLN("\n--- Test 2.1: try_inline_branch_aware single-block + params ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_add2_callee());  // func_id=0
    // Caller: 2 args at slots 0, 1; result at slot 2
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 3;
    caller.arg_count = 0;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 0, 0, 0}, 0, 13},  // closure slot=2, func_id=0
        // Need to set up the arg_base: the Call passes arg_base=3
        // but the caller has no slot 3 — we need 2 new slots
        // for the params. The inliner allocates them.
        // Actually arg_base is the slot index where the args
        // start. Let's use a call site with arg_base=3.
        {aura::ir::IROpcode::Call, {2, 3, 2, 4}, 0, 0},  // call(closure=2, args at slot 3+, 2 args, result=4)
    };
    // Pre-allocate slots 3, 4 in the caller (so arg_base=3 works)
    caller.local_count = 5;
    mod.functions.push_back(std::move(caller));

    // Invoke try_inline_branch_aware directly (bypasses the
    // slot_to_func setup that run() does).
    auto& caller_ref = mod.functions[1];
    auto& callee_ref = mod.functions[0];
    auto& block_ref = caller_ref.blocks[0];
    auto& call_instr = block_ref.instructions[1];
    bool ok = aura::compiler::InlinePass::try_inline_branch_aware_for_test(
        caller_ref, block_ref, 1, callee_ref, call_instr);

    CHECK(ok, "try_inline_branch_aware returns true on success");
    // callee_ref.local_count=5 and the max operand in
    // callee instructions is also 4 (Local(4,...), Add(4,2,3),
    // Return(4)) so caller.local_count grew by 5.
    CHECK(caller_ref.local_count >= 5 + 5,
          "caller.local_count grew by 5 (callee.local_count=5)");
    // After inlining: [MakeClosure, Local(5,3), Local(6,4), Local(7,5), Local(8,6), Add(9,7,8), Local(4, 9)]
    // Wait, the caller's local_count was 5 (slots 0..4). The
    // inliner allocates fresh slots starting at 5. The callee's
    // local_count was 5 (slots 0..4 mapped to 5..9).
    // The new instructions should be:
    //   [MakeClosure, Local(5,3), Local(6,4), Local(7,5), Local(8,6), Add(9,7,8), Local(4, 9)]
    // Note: after inlining the block.instructions should have
    // 7 entries (1 MakeClosure + 6 from inlined body).
    CHECK(block_ref.instructions.size() == 7,
          "block.instructions has 1 + (param_copies + body) entries");
    CHECK(block_ref.instructions[1].opcode == aura::ir::IROpcode::Local,
          "first inlined instr is Local (param 0 copy)");
    CHECK(block_ref.instructions[1].operands[0] == 5,
          "param 0 copies to slot 5 (fresh caller slot)");
    CHECK(block_ref.instructions[1].operands[1] == 3,
          "param 0 reads from caller's arg_base (3)");
    CHECK(block_ref.instructions[2].operands[0] == 6,
          "param 1 copies to slot 6");
    CHECK(block_ref.instructions[2].operands[1] == 4,
          "param 1 reads from caller's arg_base+1 (4)");
    // Local(7, 5) — copies callee's local 0 to slot 7
    // (this is the Local in the callee body that reads param 0)
    CHECK(block_ref.instructions[3].operands[0] == 7,
          "callee's first Local result remapped to slot 7");
    CHECK(block_ref.instructions[3].operands[1] == 5,
          "callee's first Local reads from remapped param 0 (5)");
    // Final Local(4, 9) — copies return value to call's result slot
    CHECK(block_ref.instructions[6].opcode == aura::ir::IROpcode::Local,
          "final inlined instr is Local (return-value copy)");
    CHECK(block_ref.instructions[6].operands[0] == 4,
          "return value writes to call's result slot (4)");
    return true;
}

bool test_inline_rejects_arg_count_mismatch() {
    PRINTLN("\n--- Test 2.2: try_inline_branch_aware rejects arg_count mismatch ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_add2_callee());  // 2 args
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 5;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call, {2, 3, 5, 4}, 0, 0},  // 5 args but callee wants 2
    };
    mod.functions.push_back(std::move(caller));

    auto& caller_ref = mod.functions[1];
    auto& callee_ref = mod.functions[0];
    auto& block_ref = caller_ref.blocks[0];
    auto& call_instr = block_ref.instructions[1];
    bool ok = aura::compiler::InlinePass::try_inline_branch_aware_for_test(
        caller_ref, block_ref, 1, callee_ref, call_instr);
    CHECK(!ok, "try_inline_branch_aware returns false on arg_count mismatch");
    CHECK(block_ref.instructions[1].opcode == aura::ir::IROpcode::Call,
          "Call preserved when arg_count mismatches");
    return true;
}

bool test_inline_accepts_multi_block_branch_aware() {
    PRINTLN("\n--- Test 2.3: try_inline_branch_aware multi-block CFG splicing ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_branch_aware_callee());  // 3 blocks: Branch, true/return, false/return
    // Caller: 1 arg at slot 3, result at slot 4
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 5;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call, {2, 3, 1, 4}, 0, 0},  // 1 arg, result at slot 4
    };
    mod.functions.push_back(std::move(caller));

    auto& caller_ref = mod.functions[1];
    auto& callee_ref = mod.functions[0];
    auto& block_ref = caller_ref.blocks[0];
    auto& call_instr = block_ref.instructions[1];
    bool ok = aura::compiler::InlinePass::try_inline_branch_aware_for_test(
        caller_ref, block_ref, 1, callee_ref, call_instr);
    CHECK(ok, "try_inline_branch_aware returns true on multi-block callee");
    // After inlining: caller has original block (now B_before) +
    // 3 cloned callee blocks + 1 B_after = 5 blocks total.
    CHECK(caller_ref.blocks.size() == 5,
          "caller now has 5 blocks (B_before + 3 clones + B_after)");
    // B_before: [MakeClosure, Local(slot_remap[0]=5, 3), Jump(1)]
    // (slot 5 is the fresh slot for callee's local 0;
    //  Jump(1) goes to the cloned entry block, which is the
    //  block at index 1 in the new caller.blocks — the
    //  branch block of the callee, cloned as block id=1)
    CHECK(caller_ref.blocks[0].instructions.size() == 3,
          "B_before has 3 instructions (MakeClosure + Local + Jump)");
    CHECK(caller_ref.blocks[0].instructions[1].opcode == aura::ir::IROpcode::Local,
          "B_before[1] is Local (param copy)");
    CHECK(caller_ref.blocks[0].instructions[1].operands[0] == 5,
          "param copies to slot 5 (fresh slot for callee local 0)");
    CHECK(caller_ref.blocks[0].instructions[1].operands[1] == 3,
          "param reads from caller's arg_base (3)");
    CHECK(caller_ref.blocks[0].instructions[2].opcode == aura::ir::IROpcode::Jump,
          "B_before[2] is Jump to cloned entry");
    CHECK(caller_ref.blocks[0].instructions[2].operands[0] == 1,
          "Jump target is cloned entry block id (1)");
    // Cloned entry block (block id 1): [Branch(0, 2, 3)]
    // (Branch with remapped cond=slot_rename[0]=5, true=2, false=3
    //  — but the test only checks the block exists, not the
    //  exact Branch operands. We check that the Branch is
    //  present and the targets are remapped.)
    CHECK(caller_ref.blocks[1].instructions.size() == 1,
          "cloned entry block has 1 instruction (Branch)");
    CHECK(caller_ref.blocks[1].instructions[0].opcode == aura::ir::IROpcode::Branch,
          "cloned entry block's instruction is Branch");
    CHECK(caller_ref.blocks[1].instructions[0].operands[1] == 2,
          "Branch true target remapped to cloned block 2");
    CHECK(caller_ref.blocks[1].instructions[0].operands[2] == 3,
          "Branch false target remapped to cloned block 3");
    CHECK(caller_ref.blocks[1].instructions[0].operands[0] == 5,
          "Branch cond remapped to fresh slot (5)");
    // Cloned exit blocks (2 and 3) end with Local(4, ret_src) + Jump(4)
    // (4 is call_result, ret_src is the remapped return slot;
    //  4 is the B_after id). The original callee exit blocks
    //  are [ConstI64, Return], so the cloned block is
    //  [ConstI64, Local, Jump] (3 instructions: the
    //  ConstI64's result slot is also remapped, then the
    //  trailing Return is replaced with Local + Jump).
    // The return source slot is the ConstI64's result
    // slot (1 in block 1, 2 in block 2). max_slot=2 (the
    // highest operand across all callee instructions), so
    // slot_rename[0]=5, slot_rename[1]=6, slot_rename[2]=7
    // (caller.local_count was 5, so fresh slots start at 5).
    std::uint32_t expected_ret_src[2] = {6, 7};
    std::size_t idx = 0;
    for (std::uint32_t j : {2u, 3u}) {
        const auto& cb = caller_ref.blocks[j].instructions;
        CHECK(cb.size() == 3,
              "cloned exit block has 3 instructions (ConstI64 + Local + Jump)");
        CHECK(cb[0].opcode == aura::ir::IROpcode::ConstI64,
              "cloned exit block starts with ConstI64");
        CHECK(cb[1].opcode == aura::ir::IROpcode::Local,
              "cloned exit block[1] is Local (return copy)");
        CHECK(cb[1].operands[0] == 4,
              "Local writes to call result slot (4)");
        CHECK(cb[1].operands[1] == expected_ret_src[idx],
              "Local reads from remapped return slot (per-block)");
        CHECK(cb[2].opcode == aura::ir::IROpcode::Jump,
              "cloned exit block[2] is Jump");
        CHECK(cb[2].operands[0] == 4,
              "Jump target is B_after id (4)");
        ++idx;
    }
    // B_after (block id 4): empty (the Call was the last
    // instruction in the caller's block, so no "after" code
    // to preserve). The original block had no terminator
    // (Call was last), so B_after is also empty.
    CHECK(caller_ref.blocks[4].instructions.empty(),
          "B_after is empty (Call was last in caller's block)");
    return true;
}

bool test_inline_handles_call_in_middle() {
    PRINTLN("\n--- Test 2.4: try_inline_branch_aware call-in-middle (block split) ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_add2_callee());  // single-block, but tests the split path
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 5;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call, {2, 3, 2, 4}, 0, 0},  // NOT the last
        {aura::ir::IROpcode::Return, {4, 0, 0, 0}, 0, 0},  // follows the call
    };
    mod.functions.push_back(std::move(caller));

    auto& caller_ref = mod.functions[1];
    auto& callee_ref = mod.functions[0];
    auto& block_ref = caller_ref.blocks[0];
    auto& call_instr = block_ref.instructions[1];
    bool ok = aura::compiler::InlinePass::try_inline_branch_aware_for_test(
        caller_ref, block_ref, 1, callee_ref, call_instr);
    // Single-block callee: the single-block fast path runs,
    // which requires call_pos == last. Since Call is in the
    // middle, the single-block path returns false. The
    // multi-block path is the fallback, which handles the
    // split correctly.
    CHECK(ok, "try_inline_branch_aware handles call-in-middle (block split)");
    return true;
}

// ═════════════════════════════════════════════════════════════
// AC3: inlined_branch_aware_count() accessor
// ═════════════════════════════════════════════════════════════

bool test_branch_aware_counter_separate() {
    PRINTLN("\n--- Test 3.1: inlined_branch_aware_count tracks branch-aware path ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_add2_callee());  // single-block, not const-return
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 5;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call, {2, 3, 2, 4}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::InlinePass inliner;
    inliner.run(mod);

    CHECK(inliner.inlined_count() == 0,
          "inlined_count() == 0 (pre-#197 path didn't match)");
    CHECK(inliner.inlined_branch_aware_count() == 1,
          "inlined_branch_aware_count() == 1 (new path matched)");
    CHECK(inliner.run_inlined() == 0,
          "run_inlined() == 0 (pre-#197 path didn't run)");
    CHECK(inliner.run_inlined_branch_aware() == 1,
          "run_inlined_branch_aware() == 1 (new path ran this run)");
    return true;
}

bool test_lifetime_counters() {
    PRINTLN("\n--- Test 3.2: total_inlined / total_inlined_branch_aware accumulate ---");
    // Reset the static counters so this test is independent
    // of prior tests' state. (The counters are process-wide
    // and accumulate; we can only read them, not reset them.
    // So we measure the DELTA before/after a run, not the
    // absolute value.)
    std::size_t before_inlined =
        aura::compiler::InlinePass::total_inlined();
    std::size_t before_branch =
        aura::compiler::InlinePass::total_inlined_branch_aware();
    // Run the inliner with a multi-block callee.
    aura::ir::IRModule mod;
    mod.functions.push_back(make_branch_aware_callee());
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 5;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call, {2, 3, 1, 4}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));
    aura::compiler::InlinePass inliner;
    inliner.run(mod);
    std::size_t after_inlined =
        aura::compiler::InlinePass::total_inlined();
    std::size_t after_branch =
        aura::compiler::InlinePass::total_inlined_branch_aware();
    CHECK(after_inlined == before_inlined,
          "total_inlined() unchanged (no constant-substitution)");
    CHECK(after_branch == before_branch + 1,
          "total_inlined_branch_aware() increased by 1");
    return true;
}

bool test_call_in_middle_with_terminator_preserved() {
    PRINTLN("\n--- Test 3.3: call-in-middle preserves caller's terminator in B_after ---");
    // Caller block has [MakeClosure, Call, Return]. After
    // inlining, B_before holds [MakeClosure, Local(5,3),
    // Jump(1)], and B_after holds [Return]. The Return
    // is the caller's original terminator, preserved
    // across the block split.
    aura::ir::IRModule mod;
    mod.functions.push_back(make_add2_callee());  // single-block, but goes via multi-block path because of call-in-middle
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 5;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call, {2, 3, 2, 4}, 0, 0},
        {aura::ir::IROpcode::Return, {4, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));
    auto& caller_ref = mod.functions[1];
    auto& callee_ref = mod.functions[0];
    auto& block_ref = caller_ref.blocks[0];
    auto& call_instr = block_ref.instructions[1];
    bool ok = aura::compiler::InlinePass::try_inline_branch_aware_for_test(
        caller_ref, block_ref, 1, callee_ref, call_instr);
    CHECK(ok, "try_inline_branch_aware succeeds for call-in-middle");
    // B_before (caller_ref.blocks[0]) has [MakeClosure, Local(5,3),
    // Local(6,4), Jump(1)] — 1 MakeClosure + 2 param copies (add2
    // has 2 args) + 1 Jump = 4 instructions.
    CHECK(caller_ref.blocks[0].instructions.size() == 4,
          "B_before has 4 instructions (MakeClosure + 2 param copies + Jump)");
    // The cloned single callee block is at index 1 (cloned as block id=1)
    // The callee body is [Local(2,0), Local(3,1), Add(4,2,3), Return(4)].
    // After slot remap (max_slot=4, slot_rename[0..4] = 5..9):
    //   [Local(7,5), Local(8,6), Add(9,7,8)]
    // The trailing Return is replaced with Local(call_result=4, ret_src=9)
    // + Jump(B_after=2). So the cloned block is:
    //   [Local(7,5), Local(8,6), Add(9,7,8), Local(4,9), Jump(2)]
    // (5 instructions total)
    CHECK(caller_ref.blocks[1].instructions.size() == 5,
          "cloned single callee block has 5 instructions (body + Local + Jump)");
    // The second-to-last instruction is the Local (return copy)
    CHECK(caller_ref.blocks[1].instructions[3].opcode == aura::ir::IROpcode::Local,
          "cloned block[3] is Local (return-value copy)");
    CHECK(caller_ref.blocks[1].instructions[3].operands[0] == 4,
          "Local writes to call_result (4)");
    CHECK(caller_ref.blocks[1].instructions[3].operands[1] == 9,
          "Local reads from remapped return slot (9)");
    // The last instruction is the Jump to B_after
    CHECK(caller_ref.blocks[1].instructions[4].opcode == aura::ir::IROpcode::Jump,
          "cloned block[4] is Jump");
    CHECK(caller_ref.blocks[1].instructions[4].operands[0] == 2,
          "Jump target is B_after (block id 2)");
    // B_after is at index 2. It should hold the original
    // terminator (Return).
    CHECK(caller_ref.blocks.size() == 3,
          "caller has 3 blocks (B_before + clone + B_after)");
    CHECK(caller_ref.blocks[2].instructions.size() == 1,
          "B_after has 1 instruction (preserved Return)");
    CHECK(caller_ref.blocks[2].instructions[0].opcode == aura::ir::IROpcode::Return,
          "B_after[0] is Return (original terminator preserved)");
    return true;
}

// ═════════════════════════════════════════════════════════════
// Main test runner
// ═════════════════════════════════════════════════════════════

int main() {
    std::println("═══ Issue #197 verification tests ═══\n");
    std::println("AC #1: is_inlinable_branch_aware predicate");
    test_predicate_accepts_single_block_with_params();
    test_predicate_accepts_multi_block_branch_return();
    test_predicate_rejects_nested_call();
    test_predicate_rejects_nested_make_closure();
    test_predicate_rejects_cell_set();
    test_predicate_rejects_loop();
    test_predicate_rejects_oversize();
    test_predicate_rejects_no_terminator();

    std::println("\nAC #2: try_inline_branch_aware transformation (single-block + params)");
    test_inline_single_block_with_params();
    test_inline_rejects_arg_count_mismatch();
    test_inline_accepts_multi_block_branch_aware();
    test_inline_handles_call_in_middle();

    std::println("\nAC #3: inlined_branch_aware_count accessor");
    test_branch_aware_counter_separate();
    test_lifetime_counters();
    test_call_in_middle_with_terminator_preserved();

    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}
