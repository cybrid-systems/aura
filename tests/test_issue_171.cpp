// test_issue_171.cpp — Issue #171: High-Impact IR Optimization Passes
// (Function Inliner + TCO).
//
// Issue #171 Priority 2: Function Inliner. The InlinePass
// (pass_manager.ixx) detects "trivial" callees (single-block,
// constant-returning) and inlines them at the call site. This
// test verifies the inlining actually happens (not just counts
// call sites, as the #160 scaffold did).
//
// Test scenarios:
//   1. Trivial callee is inlined at a known-static call site
//   2. Non-trivial callee is NOT inlined
//   3. Direct recursion is NOT inlined (recursion guard)
//   4. Dynamic callee (Call slot not bound to a known func_id)
//      is NOT inlined
//   5. inlined_count() reports the actual substitution count

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

// Helper: build a trivial callee that returns a constant I64
static aura::ir::IRFunction make_const_i64_callee(const std::string& name,
                                                  std::uint32_t result_slot,
                                                  int64_t value) {
    aura::ir::IRFunction func;
    func.name = name;
    func.local_count = result_slot + 1;
    func.arg_count = 0;
    func.blocks.push_back({0});
    func.blocks[0].instructions = {
        {aura::ir::IROpcode::ConstI64, {result_slot,
                                         static_cast<std::uint32_t>(value & 0xFFFFFFFF),
                                         static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFF),
                                         0}, 0, 1},
        {aura::ir::IROpcode::Return,   {result_slot, 0, 0, 0}, 0, 0},
    };
    return func;
}

// ── Test 1: trivial callee is inlined at a known-static call site ──
// The caller sets up the callee via MakeClosure(func_id=1), then
// calls via the slot. After InlinePass runs, the Call should be
// rewritten to a ConstI64 with the constant value.
bool test_inline_trivial_callee() {
    PRINTLN("\n--- Test 1: trivial callee inlined at static call site ---");
    aura::ir::IRModule mod;
    // Callee (func_id=0): returns 42
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    // Caller (func_id=1): MakeClosure(k42) then Call
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 2;
    caller.arg_count = 0;
    caller.blocks.push_back({0});
    auto& block = caller.blocks[0];
    // closure_slot = 0, result_slot = 1
    block.instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},  // closure(slot=0, func_id=0)
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},   // call(closure=0, arg_base=0, count=0, result=1)
    };
    mod.functions.push_back(std::move(caller));

    // Run the inliner
    aura::compiler::InlinePass inliner;
    inliner.run(mod);

    // The Call should have been replaced with ConstI64(1, 42, 0, 0)
    auto& inlined_block = mod.functions[1].blocks[0];
    CHECK(inlined_block.instructions.size() == 2,
          "instruction count unchanged after inlining (rewrite in place)");
    CHECK(inlined_block.instructions[0].opcode == aura::ir::IROpcode::MakeClosure,
          "MakeClosure preserved");
    CHECK(inlined_block.instructions[1].opcode == aura::ir::IROpcode::ConstI64,
          "Call rewritten to ConstI64 (the inlined constant)");
    CHECK(inlined_block.instructions[1].operands[0] == 1,
          "inlined ConstI64 writes to caller's result slot (1)");
    CHECK(inlined_block.instructions[1].operands[1] == 42,
          "inlined ConstI64 has the callee's constant value (42)");
    CHECK(inliner.inlined_count() == 1,
          "inlined_count() == 1 (one substitution)");
    return true;
}

// ── Test 2: non-trivial callee is NOT inlined ──
// A callee with a Branch (multi-block) shouldn't be inlined.
bool test_skip_non_trivial() {
    PRINTLN("\n--- Test 2: non-trivial callee NOT inlined ---");
    aura::ir::IRModule mod;
    // Callee: multi-block (Branch) — NOT inlinable
    {
        aura::ir::IRFunction func;
        func.name = "multi";
        func.local_count = 2;
        func.blocks.push_back({0});
        func.blocks.push_back({1});
        func.blocks[0].instructions = {
            {aura::ir::IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
            {aura::ir::IROpcode::Branch,   {0, 0, 1, 0}, 0, 0},
        };
        func.blocks[1].instructions = {
            {aura::ir::IROpcode::ConstI64, {1, 2, 0, 0}, 0, 1},
            {aura::ir::IROpcode::Return,   {1, 0, 0, 0}, 0, 0},
        };
        mod.functions.push_back(std::move(func));
    }
    // Caller: MakeClosure + Call
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 2;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::InlinePass inliner;
    inliner.run(mod);

    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions[1].opcode == aura::ir::IROpcode::Call,
          "Call is preserved (non-trivial callee not inlined)");
    CHECK(inliner.inlined_count() == 0,
          "inlined_count() == 0 (no substitution)");
    return true;
}

// ── Test 3: direct recursion guard ──
// A function that calls itself via a closure should NOT be
// inlined into itself (would cause infinite code growth).
bool test_skip_recursion() {
    PRINTLN("\n--- Test 3: direct recursion NOT inlined (recursion guard) ---");
    aura::ir::IRModule mod;
    // Callee (func_id=0): k42, trivial
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    // Caller (func_id=1) tries to call func_id=1 (itself)
    // via a MakeClosure. But the lowering can't actually
    // emit MakeClosure(self) — this is a synthetic test.
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 2;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 1, 0, 0}, 0, 13},  // self-closure
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},    // call self
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::InlinePass inliner;
    inliner.run(mod);

    // The Call refers to func_id=1, but caller_fid=1, so skip
    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions[1].opcode == aura::ir::IROpcode::Call,
          "self-recursive Call NOT inlined");
    CHECK(inliner.inlined_count() == 0,
          "inlined_count() == 0 (recursion guard prevented inline)");
    return true;
}

// ── Test 4: dynamic callee (slot not bound to a known func_id) ──
// A Call whose callee slot was NOT set by a MakeClosure in
// the caller shouldn't be inlined.
bool test_skip_dynamic_callee() {
    PRINTLN("\n--- Test 4: dynamic callee NOT inlined (no slot-to-func) ---");
    aura::ir::IRModule mod;
    // Callee (func_id=0): k42
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    // Caller: skip MakeClosure; Call uses slot 0 which was
    // never written to a closure.
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 2;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        // No MakeClosure — slot 0 is "dynamic"
        {aura::ir::IROpcode::Call, {0, 0, 0, 1}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::InlinePass inliner;
    inliner.run(mod);

    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions[0].opcode == aura::ir::IROpcode::Call,
          "Call with dynamic callee NOT inlined");
    CHECK(inliner.inlined_count() == 0,
          "inlined_count() == 0 (no static callee info)");
    return true;
}

// ── Test 5: multiple call sites in the same block ──
// Two calls to k42 in the same block should both be inlined.
bool test_multiple_call_sites() {
    PRINTLN("\n--- Test 5: multiple call sites both inlined ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 3;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},  // result 1
        {aura::ir::IROpcode::Call,        {0, 0, 0, 2}, 0, 0},  // result 2
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::InlinePass inliner;
    inliner.run(mod);

    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions[1].opcode == aura::ir::IROpcode::ConstI64,
          "first Call inlined to ConstI64");
    CHECK(block.instructions[1].operands[0] == 1,
          "first inline writes to slot 1");
    CHECK(block.instructions[2].opcode == aura::ir::IROpcode::ConstI64,
          "second Call inlined to ConstI64");
    CHECK(block.instructions[2].operands[0] == 2,
          "second inline writes to slot 2");
    CHECK(inliner.inlined_count() == 2,
          "inlined_count() == 2 (two substitutions)");
    return true;
}

// ── Test 6: inlined_count is per-pass (resets on each run) ──
// First run inlines 1 call. Second run sees no remaining Calls
// (they've all been rewritten to ConstXxx), so the count is 0.
// This verifies (a) the counter resets per-run, and (b) the
// inliner is idempotent (running it twice is a no-op the
// second time).
bool test_inlined_count_resets() {
    PRINTLN("\n--- Test 6: inlined_count resets between runs ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 2;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::InlinePass inliner;
    inliner.run(mod);
    CHECK(inliner.inlined_count() == 1, "first run: 1 inlined");

    inliner.run(mod);
    CHECK(inliner.inlined_count() == 0,
          "second run: 0 inlined (Call already rewritten to ConstI64, idempotent)");
    return true;
}

// ── TCO tests (Issue #171 Priority 3) ──

// ── Test 7: TCO rewrites tail Call+Return to Jump ──
bool test_tco_tail_call_rewrite() {
    PRINTLN("\n--- Test 7: TCO rewrites tail Call+Return to Jump ---");
    aura::ir::IRModule mod;
    // Callee (func_id=0): k42 (single-block trivial)
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    // Callee (func_id=1): something to TCO into. Single block:
    // Local(0, 0) ; Return(0) — but the simplest tail-call
    // pattern is Call to k42, then Return(k42_result).
    // For TCO to fire: Block ends with Call+Return.
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 2;
    caller.blocks.push_back({0});
    // MakeClosure(k42) at slot 0; Call(closure=0, args=0, count=0, result=1);
    // Return(result=1)
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},
        {aura::ir::IROpcode::Return,      {1, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::TCOPass tco;
    tco.run(mod);

    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions.size() == 2,
          "Call+Return collapsed to 2 instructions (Call → Jump, Return removed)");
    CHECK(block.instructions[0].opcode == aura::ir::IROpcode::MakeClosure,
          "MakeClosure preserved");
    CHECK(block.instructions[1].opcode == aura::ir::IROpcode::Jump,
          "tail Call rewritten to Jump");
    CHECK(block.instructions[1].operands[0] == 0,
          "Jump target = callee's entry block id (0)");
    CHECK(tco.tco_count() == 1, "tco_count() == 1");
    return true;
}

// ── Test 8: TCO skips non-tail calls ──
// A Call NOT followed by a Return (e.g., followed by another
// instruction) should not be TCO'd.
bool test_tco_skip_non_tail() {
    PRINTLN("\n--- Test 8: TCO skips non-tail calls ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 2;
    caller.blocks.push_back({0});
    // Call NOT followed by Return (followed by another ConstI64)
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},  // NOT tail
        {aura::ir::IROpcode::ConstI64,    {1, 99, 0, 0}, 0, 1},  // overwrite result
        {aura::ir::IROpcode::Return,      {1, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::TCOPass tco;
    tco.run(mod);

    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions[1].opcode == aura::ir::IROpcode::Call,
          "non-tail Call NOT rewritten (preserved as Call)");
    CHECK(tco.tco_count() == 0, "tco_count() == 0 (no tail call found)");
    return true;
}

// ── Test 9: TCO skips when Return's value != Call's result ──
bool test_tco_skip_mismatched_return() {
    PRINTLN("\n--- Test 9: TCO skips when Return's value != Call's result ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 3;
    caller.blocks.push_back({0});
    // Call returns to slot 1, but Return uses slot 2 (different)
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::ConstI64,    {2, 0, 0, 0}, 0, 1},  // separate value
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},  // result 1, but Return uses 2
        {aura::ir::IROpcode::Return,      {2, 0, 0, 0}, 0, 0},  // Return slot 2, not 1
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::TCOPass tco;
    tco.run(mod);

    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions[2].opcode == aura::ir::IROpcode::Call,
          "mismatched Call/Return NOT rewritten (Call value not used)");
    CHECK(tco.tco_count() == 0, "tco_count() == 0");
    return true;
}

// ── Test 10: TCO is idempotent ──
// Running TCO twice doesn't double-transform.
bool test_tco_idempotent() {
    PRINTLN("\n--- Test 10: TCO is idempotent ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 2;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},
        {aura::ir::IROpcode::Return,      {1, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::TCOPass tco;
    tco.run(mod);
    CHECK(tco.tco_count() == 1, "first run: 1 TCO");
    tco.run(mod);
    CHECK(tco.tco_count() == 0,
          "second run: 0 TCO (no remaining Call+Return pattern)");
    return true;
}

// ── Test 11 (Issue #201): TCO with non-zero arg_base (single arg) ──
// Callee expects param at slot 0; caller passes it from slot 5.
// TCO must emit Local(0, 5) before the Jump.
bool test_tco_arg_base_nonzero_single() {
    PRINTLN("\n--- Test 11: TCO arg_base=5 single arg ---");
    aura::ir::IRModule mod;
    // Callee (func_id=0): k42
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    // Caller
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 8;
    caller.blocks.push_back({0});
    // Caller stack:
    //   slot 0: closure (MakeClosure result)
    //   slot 5: the actual arg value
    // Call(closure=0, arg_base=5, count=1, result=6)
    // Return(6)
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::ConstI64,    {5, 7, 0, 0}, 0, 1},    // load arg
        {aura::ir::IROpcode::Call,        {0, 5, 1, 6}, 0, 0},    // tail call
        {aura::ir::IROpcode::Return,      {6, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::TCOPass tco;
    tco.run(mod);

    auto& block = mod.functions[1].blocks[0];
    // After TCO: 4 instructions: MakeClosure, ConstI64 (load 5),
    // Local(0, 5) (inserted by TCO), Jump (was Call), Return was popped.
    CHECK(block.instructions.size() == 4,
          "arg_base=5 single arg: 4 instructions (was 4: MakeClosure + ConstI64 + Local-insert + Jump)");
    CHECK(block.instructions[0].opcode == aura::ir::IROpcode::MakeClosure,
          "MakeClosure preserved");
    CHECK(block.instructions[1].opcode == aura::ir::IROpcode::ConstI64,
          "ConstI64 (load arg) preserved");
    CHECK(block.instructions[2].opcode == aura::ir::IROpcode::Local,
          "TCO inserted Local copy");
    CHECK(block.instructions[2].operands[0] == 0,
          "Local writes to callee param slot 0");
    CHECK(block.instructions[2].operands[1] == 5,
          "Local reads from caller arg_base slot 5");
    CHECK(block.instructions[3].opcode == aura::ir::IROpcode::Jump,
          "Call rewritten to Jump");
    CHECK(block.instructions[3].operands[0] == 0,
          "Jump target = callee entry block 0");
    CHECK(tco.tco_count() == 1, "tco_count() == 1");
    return true;
}

// ── Test 12 (Issue #201): TCO with non-zero arg_base, multi-arg ──
// Callee expects params at slots 0, 1, 2; caller passes them
// from slots 5, 6, 7. TCO must emit 3 Local copies before the
// Jump. After the rewrite, the block layout is:
//   [0] MakeClosure(0, 0)
//   [1] ConstI64(5, 1)
//   [2] ConstI64(6, 2)
//   [3] ConstI64(7, 3)
//   [4] Local(0, 5)   <-- inserted by TCO
//   [5] Local(1, 6)   <-- inserted by TCO
//   [6] Local(2, 7)   <-- inserted by TCO
//   [7] Jump(0)       <-- was Call, rewritten
// (Return was at [5] in the original 6-instr block; after the
// 3 Local inserts, Return moves to [8] and is then popped.)
bool test_tco_arg_base_nonzero_multi() {
    PRINTLN("\n--- Test 12: TCO arg_base=5 multi-arg (3 args) ---");
    aura::ir::IRModule mod;
    // Callee (func_id=0): k42 (3 params, single block)
    aura::ir::IRFunction k42;
    k42.name = "k42";
    k42.local_count = 3;
    k42.blocks.push_back({0});
    k42.blocks[0].instructions = {
        {aura::ir::IROpcode::Local, {0, 0, 0, 0}, 0, 0},
        {aura::ir::IROpcode::Local, {1, 1, 0, 0}, 0, 0},
        {aura::ir::IROpcode::Local, {2, 2, 0, 0}, 0, 0},
        {aura::ir::IROpcode::Return, {0, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(k42));
    // Caller
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 10;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::ConstI64,    {5, 1, 0, 0}, 0, 1},
        {aura::ir::IROpcode::ConstI64,    {6, 2, 0, 0}, 0, 1},
        {aura::ir::IROpcode::ConstI64,    {7, 3, 0, 0}, 0, 1},
        {aura::ir::IROpcode::Call,        {0, 5, 3, 8}, 0, 0},
        {aura::ir::IROpcode::Return,      {8, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::TCOPass tco;
    tco.run(mod);

    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions.size() == 8,
          "arg_base=5 multi-arg: 8 instructions (4 caller prefix + 3 Local-inserts + 1 Jump)");
    // 4 caller prefix
    CHECK(block.instructions[0].opcode == aura::ir::IROpcode::MakeClosure,
          "[0] MakeClosure preserved");
    CHECK(block.instructions[1].opcode == aura::ir::IROpcode::ConstI64,
          "[1] ConstI64(5) preserved");
    CHECK(block.instructions[2].opcode == aura::ir::IROpcode::ConstI64,
          "[2] ConstI64(6) preserved");
    CHECK(block.instructions[3].opcode == aura::ir::IROpcode::ConstI64,
          "[3] ConstI64(7) preserved");
    // 3 Local inserts at positions 4, 5, 6
    CHECK(block.instructions[4].opcode == aura::ir::IROpcode::Local,
          "[4] TCO-inserted Local");
    CHECK(block.instructions[4].operands[0] == 0 && block.instructions[4].operands[1] == 5,
          "[4] Local(0, 5) — param 0 from caller slot 5");
    CHECK(block.instructions[5].opcode == aura::ir::IROpcode::Local,
          "[5] TCO-inserted Local");
    CHECK(block.instructions[5].operands[0] == 1 && block.instructions[5].operands[1] == 6,
          "[5] Local(1, 6) — param 1 from caller slot 6");
    CHECK(block.instructions[6].opcode == aura::ir::IROpcode::Local,
          "[6] TCO-inserted Local");
    CHECK(block.instructions[6].operands[0] == 2 && block.instructions[6].operands[1] == 7,
          "[6] Local(2, 7) — param 2 from caller slot 7");
    // Call (now at position 7) was rewritten to Jump
    CHECK(block.instructions[7].opcode == aura::ir::IROpcode::Jump,
          "[7] tail Call rewritten to Jump");
    CHECK(block.instructions[7].operands[0] == 0,
          "[7] Jump target = callee entry block 0");
    CHECK(tco.tco_count() == 1, "tco_count() == 1");
    return true;
}

// ── Test 13 (Issue #201): TCO with arg_base != 0 and arg_count == 0 ──
// Edge case: no args to copy. arg_base=5, arg_count=0.
// The existing arg_base==0 path is reused (no Local inserts
// needed). TCO succeeds.
bool test_tco_arg_base_nonzero_zero_args() {
    PRINTLN("\n--- Test 13: TCO arg_base=5, arg_count=0 (no copies) ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 6;
    caller.blocks.push_back({0});
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call,        {0, 5, 0, 1}, 0, 0},  // 0 args, base 5
        {aura::ir::IROpcode::Return,      {1, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::TCOPass tco;
    tco.run(mod);

    auto& block = mod.functions[1].blocks[0];
    CHECK(block.instructions.size() == 2,
          "arg_base=5, count=0: 2 instructions (no Local inserts, Return popped)");
    CHECK(block.instructions[0].opcode == aura::ir::IROpcode::MakeClosure,
          "MakeClosure preserved");
    CHECK(block.instructions[1].opcode == aura::ir::IROpcode::Jump,
          "Call rewritten to Jump (no Local inserts since count==0)");
    CHECK(tco.tco_count() == 1, "tco_count() == 1");
    return true;
}

// ── Test 14 (Issue #201): TCO preserves tail-call semantics on
// both arg_base=0 and arg_base!=0 in the same module. Ensures
// the two paths don't interfere.
bool test_tco_arg_base_mixed_in_same_module() {
    PRINTLN("\n--- Test 14: TCO mixed arg_base=0 and arg_base!=0 in same module ---");
    aura::ir::IRModule mod;
    mod.functions.push_back(make_const_i64_callee("k42", 0, 42));
    // k99: another simple callee
    mod.functions.push_back(make_const_i64_callee("k99", 1, 99));
    // Caller: block 0 has tail-call with arg_base=0 to k42;
    //         block 1 has tail-call with arg_base=5 to k99.
    aura::ir::IRFunction caller;
    caller.name = "caller";
    caller.local_count = 8;
    caller.blocks.push_back({0});
    caller.blocks.push_back({1});
    // Block 0: MakeClosure k42 (func_id 0) at slot 0; tail Call
    // with arg_base=0, count=0 to k42; Return.
    caller.blocks[0].instructions = {
        {aura::ir::IROpcode::MakeClosure, {0, 0, 0, 0}, 0, 13},
        {aura::ir::IROpcode::Call,        {0, 0, 0, 1}, 0, 0},
        {aura::ir::IROpcode::Return,      {1, 0, 0, 0}, 0, 0},
    };
    // Block 1: MakeClosure k99 (func_id 1) at slot 2; ConstI64
    // at slot 5; tail Call with arg_base=5, count=1 to k99; Return.
    caller.blocks[1].instructions = {
        {aura::ir::IROpcode::MakeClosure, {2, 1, 0, 0}, 0, 13},
        {aura::ir::IROpcode::ConstI64,    {5, 7, 0, 0}, 0, 1},
        {aura::ir::IROpcode::Call,        {2, 5, 1, 6}, 0, 0},
        {aura::ir::IROpcode::Return,      {6, 0, 0, 0}, 0, 0},
    };
    mod.functions.push_back(std::move(caller));

    aura::compiler::TCOPass tco;
    tco.run(mod);

    // Block 0: MakeClosure + Jump (2 instrs)
    auto& b0 = mod.functions[2].blocks[0];
    CHECK(b0.instructions.size() == 2,
          "block 0: 2 instrs (MakeClosure + Jump; no Local inserts for arg_base=0)");
    CHECK(b0.instructions[1].opcode == aura::ir::IROpcode::Jump,
          "block 0: tail Call rewritten to Jump");
    // Block 1: MakeClosure + ConstI64 + Local + Jump (4 instrs)
    auto& b1 = mod.functions[2].blocks[1];
    CHECK(b1.instructions.size() == 4,
          "block 1: 4 instrs (MakeClosure + ConstI64 + Local-insert + Jump)");
    CHECK(b1.instructions[2].opcode == aura::ir::IROpcode::Local,
          "block 1: TCO-inserted Local at index 2");
    CHECK(b1.instructions[2].operands[0] == 0 && b1.instructions[2].operands[1] == 5,
          "block 1: Local(0, 5) — arg_base=5 param 0");
    CHECK(b1.instructions[3].opcode == aura::ir::IROpcode::Jump,
          "block 1: tail Call rewritten to Jump");
    CHECK(tco.tco_count() == 2, "tco_count() == 2 (one per block)");
    return true;
}

int main() {
    std::fprintf(stdout, "═══ Issue #171 — Function Inliner (Priority 2) ═══\n");
    std::fprintf(stdout, "  Verifies the inliner ACTUALLY inlines, not just counts.\n\n");

    test_inline_trivial_callee();
    test_skip_non_trivial();
    test_skip_recursion();
    test_skip_dynamic_callee();
    test_multiple_call_sites();
    test_inlined_count_resets();
    test_tco_tail_call_rewrite();
    test_tco_skip_non_tail();
    test_tco_skip_mismatched_return();
    test_tco_idempotent();
    test_tco_arg_base_nonzero_single();
    test_tco_arg_base_nonzero_multi();
    test_tco_arg_base_nonzero_zero_args();
    test_tco_arg_base_mixed_in_same_module();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
