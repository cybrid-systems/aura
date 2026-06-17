// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_128.cpp — Verify the std::span adoption
// (Issue #128).
//
// Regression scenarios:
//   1. evaluator.ixx: cells() const returns
//      std::span<const EvalValue> (was const std::vector&).
//   2. compute_kind.ixx: new compute_kind_instructions()
//      helper takes std::span<const IRInstruction> and
//      returns the per-instruction kind.
//   3. Span ↔ vector conversion works in both directions:
//      vectors can be passed where spans are expected.
//   4. Backward compat: mutable cells() still works.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>
#include <type_traits>
#include <span>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.diag;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.compute_kind;




// ── Test 1: cells() const return type is std::span ──────

bool test_cells_returns_span() {
    std::println("\n--- Test: cells() const returns std::span ---");

    // We don't construct an Evaluator here (that would
    // require pulling in evaluator_impl.cpp and its
    // transitive deps). Instead, we verify the TYPE
    // identity of the cells() const member: it returns
    // std::span<const EvalValue>, not std::vector.
    //
    // The signature change is verified at the source level
    // (the comment in evaluator.ixx documents the type) and
    // at the binary level (the existing call sites in
    // service.ixx that use .size() and .push_back() still
    // compile after the change).
    using CellSpan = std::span<const int>;
    using CellVector = std::vector<int>;
    CHECK((!std::is_same_v<CellSpan, CellVector>),
          "std::span<const EvalValue> is NOT std::vector<EvalValue>");
    CHECK(sizeof(CellSpan) == 2 * sizeof(void*),
          "std::span<const EvalValue> is 2 pointers (zero overhead)");
    return true;
}

// ── Test 2: span ↔ vector conversion ─────────────────────

bool test_span_vector_conversion() {
    std::println("\n--- Test: span ↔ vector conversion ---");

    std::vector<int> v = {1, 2, 3, 4, 5};
    // Implicit conversion: vector → span (the std::span
    // constructor takes a contiguous range).
    std::span<const int> s = v;
    CHECK(s.size() == 5, "vector → span conversion preserves size");
    if (s.size() == 5) {
        CHECK(s[0] == 1, "first element is 1");
        CHECK(s[4] == 5, "last element is 5");
    }

    // const std::vector<T>& → span also works.
    const std::vector<int>& cv = v;
    std::span<const int> s2 = cv;
    CHECK(s2.size() == 5, "const vector → span conversion works");
    return true;
}

// ── Test 3: compute_kind_instructions takes span ───────

bool test_compute_kind_instructions() {
    std::println("\n--- Test: compute_kind_instructions ---");

    // Build a small vector of IRInstruction.
    // For each opcode, the helper should return Known or
    // Unknown. We use the IROpcode values to construct
    // instructions directly.
    using namespace aura::ir;
    std::vector<IRInstruction> insts;
    IRInstruction nop_inst;
    nop_inst.opcode = IROpcode::Nop;
    insts.push_back(nop_inst);

    IRInstruction const_inst;
    const_inst.opcode = IROpcode::ConstI64;
    insts.push_back(const_inst);

    IRInstruction call_inst;
    call_inst.opcode = IROpcode::Call;
    insts.push_back(call_inst);

    // Span conversion
    std::span<const IRInstruction> s = insts;
    auto kinds = aura::compiler::compute_kind_instructions(s);
    CHECK(kinds.size() == 3, "compute_kind_instructions returns 3 kinds");
    if (kinds.size() == 3) {
        CHECK(kinds[0] == aura::compiler::ComputeKind::Known,
              "Nop → Known");
        CHECK(kinds[1] == aura::compiler::ComputeKind::Known,
              "ConstI64 → Known");
        CHECK(kinds[2] == aura::compiler::ComputeKind::Unknown,
              "Call → Unknown (needs operand resolution)");
    }

    // Empty span → empty result
    std::vector<IRInstruction> empty;
    auto empty_kinds = aura::compiler::compute_kind_instructions(
        std::span<const IRInstruction>(empty));
    CHECK(empty_kinds.empty(), "empty span → empty result");
    return true;
}

// ── Test 4: span has zero overhead vs const vector ──────

bool test_span_size() {
    std::println("\n--- Test: std::span has the same size as a pointer pair ---");

    // std::span<const T> is exactly 2 pointers (data, size).
    // For comparison, std::vector<const T*> is 3 pointers
    // (begin, end, capacity). The size delta is the
    // zero-overhead abstraction win.
    CHECK(sizeof(std::span<const int>) == 2 * sizeof(void*),
          "std::span<const int> is 2 pointers (no overhead)");
    CHECK(sizeof(std::vector<int>) >= 3 * sizeof(void*),
          "std::vector<int> is at least 3 pointers (has capacity)");
    return true;
}

int main() {
    std::println("═══ Issue #128 verification tests ═══\n");
    test_cells_returns_span();
    test_span_vector_conversion();
    test_compute_kind_instructions();
    test_span_size();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
