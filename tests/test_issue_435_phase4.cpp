// @category: integration
// @reason: Structured C++ IR for SV (Issue #435 Phase 4
//          — CoverpointIR + CovergroupIR + list-based IR
//          for the same).

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.sv_ir;

namespace aura_issue_435_phase4_detail {

static std::string run_string(aura::compiler::CompilerService& cs,
                              const std::string& src) {
    auto r = cs.eval(src);
    if (!r)
        return "";
    auto& v = *r;
    if (!aura::compiler::types::is_string(v))
        return "";
    auto idx = aura::compiler::types::as_string_idx(v);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return "";
    return std::string(heap[idx]);
}

// ── AC1: C++ CoverpointIR direct construction + accessors ──
bool test_cpp_coverpoint_ir_direct() {
    std::println("\n--- AC1: C++ CoverpointIR direct construction ---");
    using aura::compiler::sv_ir::CoverpointIR;
    using aura::compiler::sv_ir::make_coverpoint;

    CoverpointIR cp = make_coverpoint("opcode",
        std::vector<std::string>{"add", "sub", "mul"});
    CHECK(cp.var == "opcode", "CoverpointIR.var == 'opcode'");
    CHECK(cp.bins.size() == 3, "3 bins");
    CHECK(cp.bins[1] == "sub", "second bin == 'sub'");

    CoverpointIR empty = make_coverpoint("flag", {});
    CHECK(empty.bins.empty(), "empty bins");

    return true;
}

// ── AC2: emit_coverpoint ──
bool test_cpp_emit_coverpoint() {
    std::println("\n--- AC2: C++ emit_coverpoint ---");
    using aura::compiler::sv_ir::emit_coverpoint;
    using aura::compiler::sv_ir::make_coverpoint;

    std::string s;

    s = emit_coverpoint(make_coverpoint("opcode",
        std::vector<std::string>{"add", "sub", "mul"}));
    CHECK(s.find("opcode : coverpoint") != std::string::npos,
          "starts with 'opcode : coverpoint'");
    CHECK(s.find("add") != std::string::npos, "contains 'add'");
    CHECK(s.find("sub") != std::string::npos, "contains 'sub'");
    CHECK(s.find("mul") != std::string::npos, "contains 'mul'");
    CHECK(s.find("}") != std::string::npos, "ends with '}'");

    s = emit_coverpoint(make_coverpoint("flag", {}));
    CHECK(s.find("/* no bins */") != std::string::npos,
          "empty bins → '/* no bins */'");

    return true;
}

// ── AC3: C++ CovergroupIR direct construction + accessors ──
bool test_cpp_covergroup_ir_direct() {
    std::println("\n--- AC3: C++ CovergroupIR direct construction ---");
    using aura::compiler::sv_ir::CovergroupIR;
    using aura::compiler::sv_ir::make_covergroup;

    CovergroupIR cg = make_covergroup("alu_cg",
        std::vector<std::string>{"opcode : coverpoint { add, sub }",
                                  "flags : coverpoint { zero, neg }"},
        "@(posedge clk)");
    CHECK(cg.name == "alu_cg", "CovergroupIR.name == 'alu_cg'");
    CHECK(cg.coverpoint_strs.size() == 2, "2 coverpoints");
    CHECK(cg.event == "@(posedge clk)", "explicit event");

    CovergroupIR def = make_covergroup("simple", {});
    CHECK(def.event.empty(), "default event is empty");

    return true;
}

// ── AC4: emit_covergroup ──
bool test_cpp_emit_covergroup() {
    std::println("\n--- AC4: C++ emit_covergroup ---");
    using aura::compiler::sv_ir::emit_covergroup;
    using aura::compiler::sv_ir::make_covergroup;

    std::string s;

    s = emit_covergroup(make_covergroup("alu_cg",
        std::vector<std::string>{"opcode : coverpoint { add }"},
        "@(posedge clk)"));
    CHECK(s.find("covergroup alu_cg") != std::string::npos,
          "starts with 'covergroup alu_cg'");
    CHECK(s.find("@(posedge clk)") != std::string::npos,
          "contains event '@(posedge clk)'");
    CHECK(s.find("opcode : coverpoint { add }") != std::string::npos,
          "contains coverpoint");
    CHECK(s.find("}") != std::string::npos, "ends with '}'");

    // Default event is @(*)
    s = emit_covergroup(make_covergroup("simple", {}));
    CHECK(s.find("@(*)") != std::string::npos,
          "empty event → '@(*)'");

    return true;
}

// ── AC5: Aura list-based IR works (backward compat for new IR) ──
bool test_list_ir_works() {
    std::println("\n--- AC5: list-based covergroup IR ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    auto s = run_string(cs,
        "(eda:emit-covergroup "
        "  (make-eda:covergroup 'alu_cg "
        "    (list (make-eda:coverpoint 'opcode '(add sub mul)))))");
    CHECK(!s.empty(), "list-based emit returns non-empty");
    CHECK(s.find("covergroup alu_cg") != std::string::npos,
          "list-based contains 'covergroup alu_cg'");
    CHECK(s.find("opcode : coverpoint") != std::string::npos,
          "list-based contains coverpoint header");
    CHECK(s.find("add") != std::string::npos,
          "list-based contains bin 'add'");

    return true;
}

int run_tests() {
    std::println("Issue #435 Phase 4 (structured C++ IR — CoverpointIR + CovergroupIR + list IR)\n");
    test_cpp_coverpoint_ir_direct();
    test_cpp_emit_coverpoint();
    test_cpp_covergroup_ir_direct();
    test_cpp_emit_covergroup();
    test_list_ir_works();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_435_phase4_detail

int aura_issue_435_phase4_run() { return aura_issue_435_phase4_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_435_phase4_run(); }
#endif