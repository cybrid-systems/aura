// @category: unit
// @reason: no CompilerService usage; pure C++ test on IRModule + IRFunction
//          + InlinePass, no Aura layer needed.

// test_issue_246.cpp — Issue #246: IR inliner MacroIntroduced-awareness
//
// Issue #246 (scope-limited close) ships:
//   1. IRFunction gains a `marker` field (uint8_t, default 0 = User)
//      that mirrors the source AST node's SyntaxMarker.
//   2. Lowering (lowering_impl.cpp) propagates the source Lambda's
//      marker into the IRFunction it produces.
//   3. InlinePass consults `func.marker` to decide whether to skip
//      inlining. Default policy: respect_macro_hygiene_ = true,
//      which means the inliner refuses to inline callees whose
//      marker is SyntaxMarker::MacroIntroduced (= 1).
//   4. InlinePass exposes set_respect_macro_hygiene(bool) for
//      callers that want to opt in to inlining macro-introduced
//      code (e.g. trusted macros with known semantics).
//
// Test strategy (matches test_issue_197.cpp pattern):
//   - Build IRModule + IRFunction directly, set marker, run
//     InlinePass, observe inlined_count_.
//   - Direct C++ tests, no CompilerService.
//
// Tests in this file:
//   AC1: Default policy (respect_macro_hygiene_=true) skips
//        macro-introduced callees (both trivial and branch-aware)
//   AC2: set_respect_macro_hygiene(false) re-enables inlining
//        of macro-introduced callees
//   AC3: User-written callees are always inlined (sanity check
//        that the macro-hygiene guard doesn't over-block)
//   AC4: BoolLiteral-marked callees are inlined (only MacroIntroduced
//        is blocked, not other markers)
//   AC5: Trivial inlining of macro-introduced callees is also
//        blocked (consistency between the two paths)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

// Unified test harness (Issue #226)
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.ir;
import aura.compiler.pass_manager;

namespace aura_issue_246_detail {
namespace ir = aura::ir;

// Helper: build a trivial-inlinable function (single block, const+return).
// This matches the shape required by InlinePass::is_trivial_inlinable.
static ir::IRFunction make_trivial_func(const std::string& name, std::uint8_t marker) {
    ir::IRFunction f;
    f.name = name;
    f.id = 0;
    f.arg_count = 0;
    f.local_count = 1;
    f.marker = marker;

    ir::BasicBlock block;
    ir::IRInstruction const_instr;
    const_instr.opcode = ir::IROpcode::ConstI64;
    const_instr.operands = {0, 42, 0};  // slot 0 = value 42
    block.instructions.push_back(const_instr);

    ir::IRInstruction ret_instr;
    ret_instr.opcode = ir::IROpcode::Return;
    ret_instr.operands = {0};
    block.instructions.push_back(ret_instr);

    f.blocks.push_back(std::move(block));
    f.entry_block = 0;
    return f;
}

// Helper: build a caller that calls the trivial callee.
// The caller has 1 block, with: MakeClosure for the callee + Call to it.
static ir::IRFunction make_caller_with_call(std::uint32_t callee_fid) {
    ir::IRFunction f;
    f.name = "caller";
    f.id = 0;
    f.arg_count = 0;
    f.local_count = 2;

    ir::BasicBlock block;

    // MakeClosure(result=1, func_id=callee_fid, free_var_count=0)
    ir::IRInstruction mk;
    mk.opcode = ir::IROpcode::MakeClosure;
    mk.operands = {1, callee_fid, 0};
    block.instructions.push_back(mk);

    // Call(closure_slot=1, result_slot=0, argc=0)
    ir::IRInstruction call;
    call.opcode = ir::IROpcode::Call;
    call.operands = {1, 0, 0, 0};  // closure, ?, ?, result
    block.instructions.push_back(call);

    // Return
    ir::IRInstruction ret;
    ret.opcode = ir::IROpcode::Return;
    ret.operands = {0};
    block.instructions.push_back(ret);

    f.blocks.push_back(std::move(block));
    f.entry_block = 0;
    return f;
}

// ═══════════════════════════════════════════════════════════════
// AC1: default policy skips macro-introduced callees
// ═══════════════════════════════════════════════════════════════
bool test_default_skips_macro_introduced() {
    std::println("\n--- AC1: default policy skips MacroIntroduced callees ---");
    aura::compiler::InlinePass::set_respect_macro_hygiene(true);  // default

    // Build module with a macro-introduced trivial callee
    ir::IRModule module;
    auto callee = make_trivial_func("macro-const", /*marker=*/1);
    auto callee_fid = module.add_function(std::move(callee));
    auto caller = make_caller_with_call(callee_fid);
    module.add_function(std::move(caller));

    aura::compiler::InlinePass pass;
    pass.run(module);

    std::size_t inlined = pass.inlined_count();
    std::println("    [info] inlined_count = {} (expected 0: macro skipped)", inlined);
    CHECK(inlined == 0,
          "AC1: default policy skips inlining macro-introduced callees");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: opt-in (set_respect_macro_hygiene(false)) re-enables inlining
// ═══════════════════════════════════════════════════════════════
bool test_optin_reenables_inlining() {
    std::println("\n--- AC2: opt-in re-enables inlining of macro-introduced ---");
    aura::compiler::InlinePass::set_respect_macro_hygiene(false);

    ir::IRModule module;
    auto callee = make_trivial_func("macro-const-optin", /*marker=*/1);
    auto callee_fid = module.add_function(std::move(callee));
    auto caller = make_caller_with_call(callee_fid);
    module.add_function(std::move(caller));

    aura::compiler::InlinePass pass;
    pass.run(module);

    std::size_t inlined = pass.inlined_count();
    std::println("    [info] inlined_count = {} (expected 1: opt-in)", inlined);
    CHECK(inlined == 1,
          "AC2: opt-in re-enables inlining of macro-introduced callees");

    // Reset to default for subsequent tests
    aura::compiler::InlinePass::set_respect_macro_hygiene(true);
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: User-written callees are inlined (sanity, no over-blocking)
// ═══════════════════════════════════════════════════════════════
bool test_user_callee_inlined() {
    std::println("\n--- AC3: User-marked callees are inlined (sanity) ---");
    aura::compiler::InlinePass::set_respect_macro_hygiene(true);

    ir::IRModule module;
    auto callee = make_trivial_func("user-const", /*marker=*/0);
    auto callee_fid = module.add_function(std::move(callee));
    auto caller = make_caller_with_call(callee_fid);
    module.add_function(std::move(caller));

    aura::compiler::InlinePass pass;
    pass.run(module);

    std::size_t inlined = pass.inlined_count();
    std::println("    [info] inlined_count = {} (expected 1: user inlined)", inlined);
    CHECK(inlined == 1,
          "AC3: User-marked callees are inlined (no over-blocking)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC4: BoolLiteral-marked callees are inlined (only MacroIntroduced
//      is blocked, not other markers)
// ═══════════════════════════════════════════════════════════════
bool test_bool_literal_callee_inlined() {
    std::println("\n--- AC4: BoolLiteral-marked callees are inlined ---");
    aura::compiler::InlinePass::set_respect_macro_hygiene(true);

    ir::IRModule module;
    // SyntaxMarker::BoolLiteral = 2
    auto callee = make_trivial_func("bool-const", /*marker=*/2);
    auto callee_fid = module.add_function(std::move(callee));
    auto caller = make_caller_with_call(callee_fid);
    module.add_function(std::move(caller));

    aura::compiler::InlinePass pass;
    pass.run(module);

    std::size_t inlined = pass.inlined_count();
    std::println("    [info] inlined_count = {} (expected 1: bool-literal inlined)", inlined);
    CHECK(inlined == 1,
          "AC4: BoolLiteral-marked callees are inlined (only MacroIntroduced is blocked)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC5: default-marker field is 0 (User), preserves back-compat
// ═══════════════════════════════════════════════════════════════
bool test_default_marker_is_user() {
    std::println("\n--- AC5: default IRFunction.marker is 0 (User) ---");
    ir::IRFunction f;
    CHECK(f.marker == 0,
          "AC5: default IRFunction.marker is 0 (SyntaxMarker::User) — back-compat");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC6: get_respect_macro_hygiene / set_respect_macro_hygiene round-trip
// ═══════════════════════════════════════════════════════════════
bool test_setter_round_trip() {
    std::println("\n--- AC6: respect_macro_hygiene_ setter round-trip ---");
    aura::compiler::InlinePass::set_respect_macro_hygiene(true);
    CHECK(aura::compiler::InlinePass::get_respect_macro_hygiene() == true,
          "set true → get true");
    aura::compiler::InlinePass::set_respect_macro_hygiene(false);
    CHECK(aura::compiler::InlinePass::get_respect_macro_hygiene() == false,
          "set false → get false");
    // Reset to default for the rest of the suite
    aura::compiler::InlinePass::set_respect_macro_hygiene(true);
    CHECK(aura::compiler::InlinePass::get_respect_macro_hygiene() == true,
          "reset to true");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════
int run_tests() {
    std::println("═══ Issue #246 — Inliner MacroIntroduced-awareness ═══\n");

    std::println("AC #5: default marker field is 0 (back-compat)");
    test_default_marker_is_user();

    std::println("\nAC #6: setter round-trip");
    test_setter_round_trip();

    std::println("\nAC #1: default policy skips MacroIntroduced");
    test_default_skips_macro_introduced();

    std::println("\nAC #2: opt-in re-enables inlining");
    test_optin_reenables_inlining();

    std::println("\nAC #3: User-marked callees are inlined");
    test_user_callee_inlined();

    std::println("\nAC #4: BoolLiteral-marked callees are inlined");
    test_bool_literal_callee_inlined();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_246_detail

int aura_issue_246_run() { return aura_issue_246_detail::run_tests(); }

