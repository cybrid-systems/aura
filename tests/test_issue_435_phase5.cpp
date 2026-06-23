// @category: integration
// @reason: Structured C++ IR for SV (Issue #435 Phase 5
//          — ConstraintIR + list IR baseline for the same).

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

namespace aura_issue_435_phase5_detail {

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

// ── AC1: C++ ConstraintIR direct construction + accessors ──
bool test_cpp_constraint_ir_direct() {
    std::println("\n--- AC1: C++ ConstraintIR direct construction ---");
    using aura::compiler::sv_ir::ConstraintIR;
    using aura::compiler::sv_ir::make_constraint;

    ConstraintIR c = make_constraint("valid_len",
        std::vector<std::string>{"len < 128", "len > 0"});
    CHECK(c.name == "valid_len", "ConstraintIR.name == 'valid_len'");
    CHECK(c.expressions.size() == 2, "2 expressions");
    CHECK(c.expressions[0] == "len < 128", "first expr == 'len < 128'");
    CHECK(c.expressions[1] == "len > 0", "second expr == 'len > 0'");

    ConstraintIR empty = make_constraint("empty", {});
    CHECK(empty.expressions.empty(), "empty expressions");

    return true;
}

// ── AC2: emit_constraint ──
bool test_cpp_emit_constraint() {
    std::println("\n--- AC2: C++ emit_constraint ---");
    using aura::compiler::sv_ir::emit_constraint;
    using aura::compiler::sv_ir::make_constraint;

    std::string s;

    s = emit_constraint(make_constraint("valid_len",
        std::vector<std::string>{"len < 128", "len > 0"}));
    CHECK(s.find("constraint valid_len") != std::string::npos,
          "starts with 'constraint valid_len'");
    CHECK(s.find("len < 128") != std::string::npos, "contains expr 1");
    CHECK(s.find("len > 0") != std::string::npos, "contains expr 2");
    CHECK(s.find(";") != std::string::npos, "contains expr separator");
    CHECK(s.find("}") != std::string::npos, "ends with '}'");

    s = emit_constraint(make_constraint("single", {"x > 0"}));
    CHECK(s.find("constraint single") != std::string::npos,
          "single-expr: starts with name");
    CHECK(s.find("x > 0") != std::string::npos, "single expr body");
    CHECK(s.find("}") != std::string::npos, "single ends with '}'");

    s = emit_constraint(make_constraint("empty", {}));
    CHECK(s.find("constraint empty") != std::string::npos,
          "empty: starts with name");
    CHECK(s.find("}") != std::string::npos,
          "empty: ends with '}'");
    // Empty body should NOT have a separator '; '
    CHECK(s.find(";") == std::string::npos,
          "empty: no separator inside braces");

    return true;
}

// ── AC3: debug_constraint ──
bool test_debug_constraint() {
    std::println("\n--- AC3: C++ debug_constraint ---");
    using aura::compiler::sv_ir::debug_constraint;
    using aura::compiler::sv_ir::make_constraint;

    std::string s = debug_constraint(make_constraint("c",
        std::vector<std::string>{"a", "b"}));
    CHECK(s.find("constraint(name=c") != std::string::npos,
          "debug prefix");
    CHECK(s.find("exprs=[a,b]") != std::string::npos,
          "debug body");

    return true;
}

// ── AC4: Aura list-based IR works ──
bool test_list_ir_works() {
    std::println("\n--- AC4: list-based constraint IR ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    auto s = run_string(cs,
        "(eda:emit-constraint "
        "  (make-eda:constraint 'valid_len "
        "    (list \"len < 128\" \"len > 0\")))");
    CHECK(!s.empty(), "list-based emit returns non-empty");
    CHECK(s.find("constraint valid_len") != std::string::npos,
          "list-based contains name");
    CHECK(s.find("len < 128") != std::string::npos,
          "list-based contains expr 1");
    CHECK(s.find("len > 0") != std::string::npos,
          "list-based contains expr 2");
    CHECK(s.find("}") != std::string::npos,
          "list-based ends with '}'");

    return true;
}

// ── AC5: predicate + accessors roundtrip via Aura ──
bool test_list_ir_predicates() {
    std::println("\n--- AC5: list-based predicate + accessors ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    // Define a constraint, query its name and expr count
    if (!cs.eval("(define c1 "
                 "  (make-eda:constraint 'len_limit "
                 "    (list \"x < 100\" \"x > 0\")))")) {
        ++g_failed;
        return false;
    }

    auto r = cs.eval("(eda:constraint? c1)");
    CHECK(r.has_value(),
          "predicate eda:constraint? returns non-empty (symbol #t / #f)");

    auto s = run_string(cs, "(eda:name-str (eda:constraint-name c1))");
    CHECK(s == "len_limit",
          "eda:constraint-name returns the name as symbol-string");

    r = cs.eval("(length (eda:constraint-expressions c1))");
    CHECK(r.has_value() && aura::compiler::types::is_int(*r)
              && aura::compiler::types::as_int(*r) == 2,
          "eda:constraint-expressions returns 2 elements");

    return true;
}

int run_tests() {
    std::println("Issue #435 Phase 5 (structured C++ IR — ConstraintIR + list IR)\n");
    test_cpp_constraint_ir_direct();
    test_cpp_emit_constraint();
    test_debug_constraint();
    test_list_ir_works();
    test_list_ir_predicates();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_435_phase5_detail

int aura_issue_435_phase5_run() { return aura_issue_435_phase5_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_435_phase5_run(); }
#endif