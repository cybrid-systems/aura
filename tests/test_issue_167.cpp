// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_167.cpp — Issue #167: IR layer SoA/DOD migration
// (Phase 1: scaffold).
//
// Verifies the basic IRModuleV2 + IRInstructionView API works:
//   1. Add functions to an IRModuleV2
//   2. Add instructions to a function (each adds to every SoA column)
//   3. View an instruction via IRInstructionView
//   4. View accessors return the correct values
//   5. Basic block range-based iteration
//   6. The view is cheap to copy (static_assert in the .ixx)

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
import aura.compiler.ir;
import aura.compiler.ir_soa;



namespace aura_issue_167_detail {
#define PRINTLN(msg) do { std::print("{}\n", std::string(msg)); } while(0)

// ── Test 1: empty IRModuleV2 ──
bool test_empty_module() {
    PRINTLN("\n--- Test 1: empty IRModuleV2 ---");
    aura::compiler::IRModuleV2 mod;
    mod.name = "test";
    CHECK(mod.functions.empty(), "empty module has 0 functions");
    CHECK(mod.string_pool.empty(), "empty module has empty string pool");
    return true;
}

// ── Test 2: add function and instructions ──
bool test_add_instructions() {
    PRINTLN("\n--- Test 2: add_instruction fills all SoA columns ---");
    aura::compiler::IRModuleV2 mod;
    mod.name = "f1";
    aura::compiler::IRFunctionSoA fn;
    mod.functions.push_back(std::move(fn));

    // Add 5 instructions
    auto i0 = mod.add_instruction(0, aura::ir::IROpcode::ConstI64,
                                  {0, 42, 0, 0});
    auto i1 = mod.add_instruction(0, aura::ir::IROpcode::ConstI64,
                                  {1, 100, 0, 0});
    auto i2 = mod.add_instruction(0, aura::ir::IROpcode::Add,
                                  {2, 0, 1, 0});
    auto i3 = mod.add_instruction(0, aura::ir::IROpcode::Local,
                                  {3, 2, 0, 0});
    auto i4 = mod.add_instruction(0, aura::ir::IROpcode::Return,
                                  {2, 0, 0, 0});

    CHECK(i0 == 0 && i1 == 1 && i2 == 2 && i3 == 3 && i4 == 4,
          "instruction indices are 0,1,2,3,4 in order");
    CHECK(mod.functions[0].size() == 5, "function has 5 instructions");
    CHECK(mod.functions[0].opcodes_.size() == 5, "opcodes_ column has 5 entries");
    CHECK(mod.functions[0].operand0_.size() == 5, "operand0_ column has 5 entries");
    CHECK(mod.functions[0].operand1_.size() == 5, "operand1_ column has 5 entries");
    CHECK(mod.functions[0].operand2_.size() == 5, "operand2_ column has 5 entries");
    CHECK(mod.functions[0].operand3_.size() == 5, "operand3_ column has 5 entries");
    CHECK(mod.functions[0].type_ids_.size() == 5, "type_ids_ column has 5 entries");
    CHECK(mod.functions[0].shape_ids_.size() == 5, "shape_ids_ column has 5 entries");
    CHECK(mod.functions[0].linear_ownership_states_.size() == 5,
          "linear_ownership_states_ column has 5 entries");
    CHECK(mod.functions[0].adt_variant_ids_.size() == 5, "adt_variant_ids_ column has 5 entries");
    CHECK(mod.functions[0].narrow_evidence_.size() == 5, "narrow_evidence_ column has 5 entries");
    return true;
}

// ── Test 3: view accessors return correct data ──
bool test_view_accessors() {
    PRINTLN("\n--- Test 3: IRInstructionView accessors return correct data ---");
    aura::compiler::IRModuleV2 mod;
    aura::compiler::IRFunctionSoA fn;
    fn.reserve(2);
    mod.functions.push_back(std::move(fn));

    mod.add_instruction(0, aura::ir::IROpcode::ConstI64,
                       {0, 42, 0, 0},
                       /*source_node_id=*/100,
                       /*type_id=*/200,
                       /*shape_id=*/300,
                       /*linear_state=*/1,
                       /*adt_variant_id=*/2,
                       /*narrow_evidence=*/4);
    mod.add_instruction(0, aura::ir::IROpcode::Add,
                       {1, 0, 1, 0}, 200, 201, 301, 0, 0, 0);

    auto v0 = mod.view_at(0, 0);
    CHECK(v0.opcode() == aura::ir::IROpcode::ConstI64,
          "v0.opcode() = ConstI64");
    CHECK(v0.operand(0) == 0, "v0.operand(0) = 0 (result slot)");
    CHECK(v0.operand(1) == 42, "v0.operand(1) = 42 (the constant value)");
    CHECK(v0.operand(2) == 0, "v0.operand(2) = 0");
    CHECK(v0.operand(3) == 0, "v0.operand(3) = 0");
    CHECK(v0.source_node_id() == 100, "v0.source_node_id() = 100");
    CHECK(v0.type_id() == 200, "v0.type_id() = 200");
    CHECK(v0.shape_id() == 300, "v0.shape_id() = 300");
    CHECK(v0.linear_ownership_state() == 1, "v0.linear_ownership_state() = 1 (Owned)");
    CHECK(v0.adt_variant_id() == 2, "v0.adt_variant_id() = 2");
    CHECK(v0.narrow_evidence() == 4, "v0.narrow_evidence() = 4");

    auto v1 = mod.view_at(0, 1);
    CHECK(v1.opcode() == aura::ir::IROpcode::Add, "v1.opcode() = Add");
    CHECK(v1.operand(0) == 1, "v1.operand(0) = 1");
    CHECK(v1.operand(1) == 0, "v1.operand(1) = 0");
    CHECK(v1.operand(2) == 1, "v1.operand(2) = 1");
    return true;
}

// ── Test 4: view is cheap to copy (16 bytes) ──
bool test_view_size() {
    PRINTLN("\n--- Test 4: IRInstructionView is small (≤ 16 bytes) ---");
    CHECK(sizeof(aura::compiler::IRInstructionView) <= 16,
          "IRInstructionView fits in 16 bytes (pointer + uint32)");
    aura::compiler::IRModuleV2 mod;
    aura::compiler::IRFunctionSoA fn;
    mod.functions.push_back(std::move(fn));
    mod.add_instruction(0, aura::ir::IROpcode::Nop, {});
    // Copy ctor + assignment
    auto v1 = mod.view_at(0, 0);
    auto v2 = v1;
    v2 = v1;
    CHECK(v1.idx == v2.idx, "copy preserves idx");
    CHECK(v1.func == v2.func, "copy preserves func pointer");
    return true;
}

// ── Test 5: basic block range-based iteration ──
bool test_block_ranges() {
    PRINTLN("\n--- Test 5: BasicBlockSoA range iteration ---");
    aura::compiler::IRModuleV2 mod;
    aura::compiler::IRFunctionSoA fn;
    mod.functions.push_back(std::move(fn));

    // Block 0: instructions 0, 1
    mod.add_block(0);
    mod.add_instruction(0, aura::ir::IROpcode::ConstI64, {0, 1, 0, 0});
    mod.add_instruction(0, aura::ir::IROpcode::ConstI64, {1, 2, 0, 0});
    mod.seal_block(0, 0);

    // Block 1: instructions 2, 3
    mod.add_block(0);
    mod.add_instruction(0, aura::ir::IROpcode::Add, {2, 0, 1, 0});
    mod.add_instruction(0, aura::ir::IROpcode::Return, {2, 0, 0, 0});
    mod.seal_block(0, 1);

    // Block 1 has successor 0 (loop back)
    mod.functions[0].blocks_[1].successors.push_back(0);

    auto& f = mod.functions[0];
    CHECK(f.blocks_.size() == 2, "function has 2 blocks");
    CHECK(f.blocks_[0].start_idx == 0 && f.blocks_[0].end_idx == 2,
          "block 0 covers instructions [0, 2)");
    CHECK(f.blocks_[1].start_idx == 2 && f.blocks_[1].end_idx == 4,
          "block 1 covers instructions [2, 4)");
    CHECK(f.blocks_[1].successors.size() == 1 && f.blocks_[1].successors[0] == 0,
          "block 1's successor is block 0");

    // Walk block 0 via the view API
    std::uint32_t sum = 0;
    for (auto i = f.blocks_[0].start_idx; i < f.blocks_[0].end_idx; ++i) {
        auto v = mod.view_at(0, i);
        sum += v.operand(1);  // sum the constant values (1 + 2 = 3)
    }
    CHECK(sum == 3, "block 0 iterates opcodes with sum of constants = 3");
    return true;
}

// ── Test 6: instruction with full metadata (all 10 columns) ──
bool test_full_metadata() {
    PRINTLN("\n--- Test 6: instruction with all 10 SoA columns populated ---");
    aura::compiler::IRModuleV2 mod;
    aura::compiler::IRFunctionSoA fn;
    mod.functions.push_back(std::move(fn));

    // Set every field
    mod.add_instruction(0, aura::ir::IROpcode::MakePair,
                       /*operands*/ {99, 7, 8, 9},
                       /*source_node_id*/ 12345,
                       /*type_id*/ 678,
                       /*shape_id*/ 901,
                       /*linear_state*/ 3,        // MutBorrowed
                       /*adt_variant_id*/ 42,
                       /*narrow_evidence*/ 0xFF);  // all bits
    auto v = mod.view_at(0, 0);
    CHECK(v.operand(0) == 99, "operand(0) = 99");
    CHECK(v.operand(1) == 7, "operand(1) = 7");
    CHECK(v.operand(2) == 8, "operand(2) = 8");
    CHECK(v.operand(3) == 9, "operand(3) = 9");
    CHECK(v.source_node_id() == 12345, "source_node_id = 12345");
    CHECK(v.type_id() == 678, "type_id = 678");
    CHECK(v.shape_id() == 901, "shape_id = 901");
    CHECK(v.linear_ownership_state() == 3, "linear_state = 3 (MutBorrowed)");
    CHECK(v.adt_variant_id() == 42, "adt_variant_id = 42");
    CHECK(v.narrow_evidence() == 0xFF, "narrow_evidence = 0xFF");
    return true;
}

int run_tests() {
    std::fprintf(stdout, "═══ Issue #167 — IR layer SoA/DOD migration (Phase 1) ═══\n");

    test_empty_module();
    test_add_instructions();
    test_view_accessors();
    test_view_size();
    test_block_ranges();
    test_full_metadata();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_167_detail

int aura_issue_167_run() { return aura_issue_167_detail::run_tests(); }

