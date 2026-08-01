// @category: unit
// @reason: Issue #2471 — correct chain-walking termination in
//          IRModule::optimize_type_info() (sentinel MAX, not remap==0).
//
//   AC1: X→0→5 multi-step chain remaps uses to terminal source (not MAX)
//   AC2: Direct remap to slot 0 (no further) keeps op=0 (slot 0 valid)
//   AC3: Existing redundant CastOp elimination still works
//   AC4: Source cites Issue #2471 + MAX sentinel termination
//   AC5: Gate wiring present

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <print>
#include <string>

import std;
import aura.compiler.ir;

namespace {

using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static int count_opcode(const IRModule& mod, IROpcode op) {
    int n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == op)
                    ++n;
    return n;
}

// ── AC1: multi-step chain X→0→5 ──
// ConstI64 into slot 5 (type Int=1).
// CastOp slot0 = cast(slot5, Int)  → remap[0]=5
// CastOp slot2 = cast(slot0, Int)  → remap[2]=0
// Return uses slot2 → must become slot5 after chain walk.
static void ac1_multistep_through_slot0() {
    std::println("\n--- #2471 AC1: multi-step remap X→0→5 ---");
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "chain", .local_count = 8, .arg_count = 0});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    auto& block = func.blocks.back();
    // ConstI64 result=5 value=42 type_id=1
    block.instructions.push_back(
        IRInstruction{IROpcode::ConstI64, {5, 42, 0, 0}, 0, /*type_id=*/1});
    // Redundant CastOp result=0 from slot5 Int→Int
    block.instructions.push_back(IRInstruction{IROpcode::CastOp, {0, 5, 1, 0}, 0, 1});
    // Redundant CastOp result=2 from slot0 Int→Int (intermediate is slot 0!)
    block.instructions.push_back(IRInstruction{IROpcode::CastOp, {2, 0, 1, 0}, 0, 1});
    // Return uses cast result slot 2
    block.instructions.push_back(IRInstruction{IROpcode::Return, {2, 0, 0, 0}, 0, 0});

    const int casts_before = count_opcode(mod, IROpcode::CastOp);
    mod.optimize_type_info();
    const int casts_after = count_opcode(mod, IROpcode::CastOp);

    CHECK(casts_before == 2, "AC1: two CastOps before");
    CHECK(casts_after == 0, "AC1: both CastOps eliminated");

    // Find Return and verify operand was remapped through chain to 5
    bool found_ret = false;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions) {
                if (i.opcode != IROpcode::Return)
                    continue;
                found_ret = true;
                CHECK(i.operands[0] == 5, "AC1: Return remapped to terminal slot 5");
                CHECK(i.operands[0] != std::numeric_limits<std::uint32_t>::max(),
                      "AC1: Return not MAX sentinel");
            }
    CHECK(found_ret, "AC1: Return present");
}

// ── AC2: direct remap to slot 0 (no further) ──
// ConstI64 into slot 0, CastOp slot 3 = cast(0, Int), Return uses 3 → must become 0.
static void ac2_terminal_is_slot0() {
    std::println("\n--- #2471 AC2: terminal source is slot 0 ---");
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "term0", .local_count = 6, .arg_count = 0});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    auto& block = func.blocks.back();
    block.instructions.push_back(IRInstruction{IROpcode::ConstI64, {0, 7, 0, 0}, 0, /*type_id=*/1});
    block.instructions.push_back(IRInstruction{IROpcode::CastOp, {3, 0, 1, 0}, 0, 1});
    block.instructions.push_back(IRInstruction{IROpcode::Return, {3, 0, 0, 0}, 0, 0});

    mod.optimize_type_info();
    CHECK(count_opcode(mod, IROpcode::CastOp) == 0, "AC2: CastOp eliminated");

    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions) {
                if (i.opcode != IROpcode::Return)
                    continue;
                CHECK(i.operands[0] == 0, "AC2: Return remapped to slot 0 (valid)");
                CHECK(i.operands[0] != std::numeric_limits<std::uint32_t>::max(), "AC2: not MAX");
            }
}

// ── AC3: simple redundant cast still eliminated ──
static void ac3_simple_elim() {
    std::println("\n--- #2471 AC3: simple CastOp elim still works ---");
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "simple", .local_count = 4, .arg_count = 0});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    auto& block = func.blocks.back();
    block.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        IRInstruction{IROpcode::CastOp, {1, 0, 1, 0}, 0, 1},
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    const int before = count_opcode(mod, IROpcode::CastOp);
    mod.optimize_type_info();
    const int after = count_opcode(mod, IROpcode::CastOp);
    CHECK(before == 1 && after == 0, "AC3: single CastOp eliminated");
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::Return)
                    CHECK(i.operands[0] == 0, "AC3: Return uses source slot 0");
}

// ── AC4: source cite ──
static void ac4_source_cite() {
    std::println("\n--- #2471 AC4: source cites sentinel termination ---");
    auto ir = read_file("src/compiler/ir.ixx");
    CHECK(ir.find("Issue #2471") != std::string::npos, "AC4: cites #2471");
    CHECK(ir.find("optimize_type_info") != std::string::npos, "AC4: optimize_type_info present");
    // Bug pattern must be gone
    CHECK(ir.find("slot_remap[src] != 0") == std::string::npos,
          "AC4: old != 0 termination removed");
    CHECK(ir.find("numeric_limits<std::uint32_t>::max()") != std::string::npos,
          "AC4: MAX sentinel used in chain walk");
    // Contiguous issue mention near chain walk
    CHECK(ir.find("Do NOT terminate on remap==0") != std::string::npos ||
              ir.find("slot 0 is a valid intermediate") != std::string::npos,
          "AC4: documents slot 0 validity");
}

// ── AC5: gate wiring ──
static void ac5_gate() {
    std::println("\n--- #2471 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/check_ir_optimize_type_info_chain_2471.py");
    CHECK(build.find("check_ir_optimize_type_info_chain_2471") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_ir_optimize_type_info_chain_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_ir_optimize_type_info_chain_2471") != std::string::npos,
          "AC5: cmake test");
    CHECK(!script.empty() && script.find("2471") != std::string::npos, "AC5: check script exists");
}

} // namespace

int main() {
    std::println("=== Issue #2471: optimize_type_info chain-walk termination ===");
    ac1_multistep_through_slot0();
    ac2_terminal_is_slot0();
    ac3_simple_elim();
    ac4_source_cite();
    ac5_gate();
    std::println("\n=== #2471 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
