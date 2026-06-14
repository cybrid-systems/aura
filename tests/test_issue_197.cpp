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

import std;
import aura.core;
import aura.compiler.ir;
import aura.compiler.pass_manager;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL: %s (line %d)\n", (msg), __LINE__); \
        ++g_failed; \
    } else { \
        std::fprintf(stdout, "  PASS: %s\n", (msg)); \
        ++g_passed; \
    } \
} while(0)

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
    CHECK(caller_ref.local_count >= 5 + callee_ref.local_count,
          "caller.local_count grew by callee.local_count");
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

bool test_inline_rejects_multi_block_for_now() {
    PRINTLN("\n--- Test 2.3: try_inline_branch_aware rejects multi-block (CFG splicing is follow-up) ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_branch_aware_callee());  // 3 blocks
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 4;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call, {2, 3, 1, 4}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    auto& caller_ref = mod.functions[1];
    auto& callee_ref = mod.functions[0];
    auto& block_ref = caller_ref.blocks[0];
    auto& call_instr = block_ref.instructions[1];
    bool ok = aura::compiler::InlinePass::try_inline_branch_aware_for_test(
        caller_ref, block_ref, 1, callee_ref, call_instr);
    CHECK(!ok, "try_inline_branch_aware returns false on multi-block callee (this PR is single-block only)");
    CHECK(block_ref.instructions[1].opcode == aura::ir::IROpcode::Call,
          "Call preserved when callee is multi-block");
    return true;
}

bool test_inline_rejects_call_in_middle() {
    PRINTLN("\n--- Test 2.4: try_inline_branch_aware rejects call-in-middle (block split is follow-up) ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_add2_callee());
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
    CHECK(!ok, "try_inline_branch_aware returns false when Call is in middle (block split is follow-up)");
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
    test_inline_rejects_multi_block_for_now();
    test_inline_rejects_call_in_middle();

    std::println("\nAC #3: inlined_branch_aware_count accessor");
    test_branch_aware_counter_separate();

    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
