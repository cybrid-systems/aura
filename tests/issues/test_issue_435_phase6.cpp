// @category: integration
// @reason: Structured C++ IR for SV (Issue #435 Phase 6
//          — ClassIR + list IR baseline for the same).
//          Final phase of the #435 umbrella.


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.sv_ir;

namespace aura_issue_435_phase6_detail {

static std::string run_string(aura::compiler::CompilerService& cs, const std::string& src) {
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

// ── AC1: C++ ClassIR direct construction + accessors ──
bool test_cpp_class_ir_direct() {
    std::println("\n--- AC1: C++ ClassIR direct construction ---");
    using aura::compiler::sv_ir::ClassIR;
    using aura::compiler::sv_ir::make_class;

    ClassIR c = make_class("packet", "base_pkt",
                           std::vector<std::string>{"logic [7:0] data;", "randc bit [3:0] len;"});
    CHECK(c.name == "packet", "ClassIR.name == 'packet'");
    CHECK(c.base == "base_pkt", "ClassIR.base == 'base_pkt'");
    CHECK(c.items.size() == 2, "2 items");
    CHECK(c.items[0] == "logic [7:0] data;", "first item");

    ClassIR root = make_class("root", "", {});
    CHECK(root.base.empty(), "empty base");
    CHECK(root.items.empty(), "empty items");

    return true;
}

// ── AC2: emit_class — with base class + items ──
bool test_cpp_emit_class_with_base() {
    std::println("\n--- AC2: C++ emit_class with base ---");
    using aura::compiler::sv_ir::emit_class;
    using aura::compiler::sv_ir::make_class;

    std::string s;

    s = emit_class(make_class("packet", "base_pkt", std::vector<std::string>{"logic [7:0] data;"}));
    CHECK(s.find("class packet") != std::string::npos, "starts with 'class packet'");
    CHECK(s.find("extends base_pkt") != std::string::npos, "contains 'extends base_pkt'");
    CHECK(s.find("logic [7:0] data;") != std::string::npos, "contains item body");
    CHECK(s.find("endclass") != std::string::npos, "ends with 'endclass'");

    return true;
}

// ── AC3: emit_class — no base class + multiple items ──
bool test_cpp_emit_class_no_base() {
    std::println("\n--- AC3: C++ emit_class no base ---");
    using aura::compiler::sv_ir::emit_class;
    using aura::compiler::sv_ir::make_class;

    std::string s;

    s = emit_class(make_class("simple", "", std::vector<std::string>{"int x;", "int y;"}));
    CHECK(s.find("class simple") != std::string::npos, "starts with 'class simple'");
    CHECK(s.find("extends") == std::string::npos, "no 'extends' when base is empty");
    CHECK(s.find("int x;") != std::string::npos, "contains 'int x;'");
    CHECK(s.find("int y;") != std::string::npos, "contains 'int y;'");
    CHECK(s.find("endclass") != std::string::npos, "ends with 'endclass'");

    return true;
}

// ── AC4: emit_class — empty body ──
bool test_cpp_emit_class_empty() {
    std::println("\n--- AC4: C++ emit_class empty ---");
    using aura::compiler::sv_ir::emit_class;
    using aura::compiler::sv_ir::make_class;

    std::string s = emit_class(make_class("empty", "", {}));
    CHECK(s.find("class empty") != std::string::npos, "starts with 'class empty'");
    CHECK(s.find("endclass") != std::string::npos, "ends with 'endclass'");

    return true;
}

// ── AC5: Aura list-based IR works ──
bool test_list_ir_works() {
    std::println("\n--- AC5: list-based class IR ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    auto s = run_string(cs, "(eda:emit-class "
                            "  (make-eda:class 'packet "
                            "    'base_pkt "
                            "    (list \"logic [7:0] data;\")))");
    CHECK(!s.empty(), "list-based emit returns non-empty");
    CHECK(s.find("class packet") != std::string::npos, "list-based contains 'class packet'");
    CHECK(s.find("extends base_pkt") != std::string::npos,
          "list-based contains 'extends base_pkt'");
    CHECK(s.find("logic [7:0] data;") != std::string::npos, "list-based contains item body");
    CHECK(s.find("endclass") != std::string::npos, "list-based ends with 'endclass'");

    return true;
}

// ── AC6: list-based predicate + accessors + heterogeneous items ──
bool test_list_ir_heterogeneous() {
    std::println("\n--- AC6: list-based heterogeneous items (constraint + string) ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    // A class with a constraint (heterogeneous items).
    auto s = run_string(cs, "(eda:emit-class "
                            "  (make-eda:class 'packet "
                            "    '() "
                            "    (list "
                            "      \"logic [7:0] data;\" "
                            "      (make-eda:constraint 'valid_len "
                            "        (list \"len < 128\")))))");
    CHECK(!s.empty(), "heterogeneous emit returns non-empty");
    CHECK(s.find("class packet") != std::string::npos, "contains 'class packet'");
    CHECK(s.find("logic [7:0] data;") != std::string::npos, "contains string item");
    CHECK(s.find("constraint valid_len") != std::string::npos, "contains routed constraint");
    CHECK(s.find("len < 128") != std::string::npos, "contains constraint expr");
    CHECK(s.find("endclass") != std::string::npos, "ends with 'endclass'");

    return true;
}

int run_tests() {
    std::println("Issue #435 Phase 6 (structured C++ IR — ClassIR + list IR — final phase)\n");
    test_cpp_class_ir_direct();
    test_cpp_emit_class_with_base();
    test_cpp_emit_class_no_base();
    test_cpp_emit_class_empty();
    test_list_ir_works();
    test_list_ir_heterogeneous();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_435_phase6_detail

int aura_issue_435_phase6_run() {
    return aura_issue_435_phase6_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_435_phase6_run();
}
#endif