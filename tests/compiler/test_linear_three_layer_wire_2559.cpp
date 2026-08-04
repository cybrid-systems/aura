// @category: unit
// @reason: Issue #2559 — three-layer linear invariant wire inventory gate
//          (type / IR / pin-remap densify).
//
//   AC1: Boundary exit cites force_linear_rollback (unified linear deny)
//   AC2: post-mutate / typed_mutate cites linear_post_mutate_enforce
//   AC3: densify Phase 5 ANDs pin_contract + root_remap + scan_fail
//   AC4: Soft densify shape (had_moving / default pin held)
//   AC5: Source inventory + #2559 cites
//   AC6: Linter --self-test + gate registration

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

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

// ── AC1 ──
static void ac1_boundary_force_linear() {
    std::println("\n--- #2559 AC1: boundary force_linear_rollback ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(!emb.empty(), "AC1: read boundary");
    CHECK(emb.find("force_linear_rollback") != std::string::npos, "AC1: force_linear_rollback");
    CHECK(emb.find("linear_synth_hard_fail") != std::string::npos ||
              emb.find("linear-synth-hard-fail") != std::string::npos,
          "AC1: synth hard-fail path");
    CHECK(emb.find("Issue #2559") != std::string::npos, "AC1: boundary cites #2559");
}

// ── AC2 ──
static void ac2_post_mutate_enforce() {
    std::println("\n--- #2559 AC2: post-mutate linear enforce ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(etc.find("linear_post_mutate_enforce_all") != std::string::npos,
          "AC2: typecheck post-mutate enforce");
    CHECK(mut.find("linear_post_mutate_enforce") != std::string::npos,
          "AC2: typed_mutate post-mutate enforce");
    CHECK(etc.find("Issue #2559") != std::string::npos || mut.find("#2559") != std::string::npos,
          "AC2: type/mutate cite #2559");
}

// ── AC3 ──
static void ac3_densify_triple_and() {
    std::println("\n--- #2559 AC3: densify pin ∧ root_remap ∧ scan_fail ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("pin_contract_held") != std::string::npos, "AC3: pin_contract_held");
    CHECK(emb.find("root_remap_stable_ref_fail_total == 0") != std::string::npos,
          "AC3: root_remap stable fail");
    CHECK(emb.find("root_remap_closure_capture_fail_total == 0") != std::string::npos,
          "AC3: root_remap closure fail");
    CHECK(emb.find("scan_fail_delta") != std::string::npos, "AC3: scan_fail_delta");
    CHECK(emb.find("Issue #2499") != std::string::npos, "AC3: #2499 lineage");
    CHECK(emb.find("Issue #2497") != std::string::npos, "AC3: #2497 lineage");
}

// ── AC4 ──
static void ac4_soft_densify_shape() {
    std::println("\n--- #2559 AC4: soft densify zero-cost shape ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("had_moving_densify") != std::string::npos, "AC4: had_moving_densify");
    CHECK(emb.find("moving_compact_enabled") != std::string::npos, "AC4: moving_compact_enabled");
    CHECK(emb.find("if (had_moving_densify && pin_contract_held)") != std::string::npos,
          "AC4: densify work gated on Moving+pin");
}

// ── AC5 ──
static void ac5_source_inventory() {
    std::println("\n--- #2559 AC5: inventory script + IR layer ---");
    const auto lint = read_file("scripts/coverage/checks/check_linear_three_layer_wire_2559.py");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(lint.find("TYPE_LAYER_SITES") != std::string::npos, "AC5: TYPE_LAYER_SITES");
    CHECK(lint.find("IR_LAYER_SITES") != std::string::npos, "AC5: IR_LAYER_SITES");
    CHECK(lint.find("MEMORY_LAYER_SITES") != std::string::npos, "AC5: MEMORY_LAYER_SITES");
    CHECK(lint.find("LINEAR_WIRE_EXEMPT") != std::string::npos, "AC5: LINEAR_WIRE_EXEMPT");
    CHECK(lint.find("try_lower_linear_type") != std::string::npos, "AC5: IR lower inventory");
    CHECK(lint.find("enforce_linear_ownership_state") != std::string::npos,
          "AC5: IR executor inventory");

    const auto llt = read_file("src/compiler/lowering_linear_types_impl.cpp");
    CHECK(llt.find("try_lower_linear_type") != std::string::npos, "AC5: try_lower_linear_type");
    const auto irx = read_file("src/compiler/ir_executor_impl.cpp");
    CHECK(irx.find("enforce_linear_ownership_state") != std::string::npos,
          "AC5: enforce_linear_ownership_state");
}

// ── AC6 ──
static void ac6_linter_self_test() {
    std::println("\n--- #2559 AC6: linter self-test + registrations ---");
    const int rc = std::system(
        "python3 scripts/coverage/checks/check_linear_three_layer_wire_2559.py --self-test");
    CHECK(rc == 0, "AC6: --self-test exit 0");
    const int rc2 =
        std::system("python3 scripts/coverage/checks/check_linear_three_layer_wire_2559.py");
    CHECK(rc2 == 0, "AC6: full linter exit 0");

    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    CHECK(cmake.find("test_linear_three_layer_wire_2559") != std::string::npos, "AC6: cmake");
    CHECK(build.find("check_linear_three_layer_wire_2559") != std::string::npos,
          "AC6: build script");
    CHECK(build.find("cmd_linear_three_layer_wire_coverage") != std::string::npos,
          "AC6: build cmd");
}

} // namespace

int run_test_linear_three_layer_wire_2559() {
    std::println("=== Issue #2559: three-layer linear wire inventory gate ===");
    ac1_boundary_force_linear();
    ac2_post_mutate_enforce();
    ac3_densify_triple_and();
    ac4_soft_densify_shape();
    ac5_source_inventory();
    ac6_linter_self_test();
    std::println("\n=== #2559: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_three_layer_wire_2559();
}
#endif
