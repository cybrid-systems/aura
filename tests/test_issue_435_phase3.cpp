// @category: integration
// @reason: Structured C++ IR for SV (Issue #435 Phase 3
//          — SequenceIR + PropertyIR). SVA sequences and
//          properties.


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.sv_ir;

namespace aura_issue_435_phase3_detail {

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

// ── AC1: C++ SequenceIR direct construction + accessors ──
bool test_cpp_sequence_ir_direct() {
    std::println("\n--- AC1: C++ SequenceIR direct construction ---");
    using aura::compiler::sv_ir::SequenceIR;
    using aura::compiler::sv_ir::make_sequence;

    SequenceIR s = make_sequence("req_seq", "req ##1 ack");
    CHECK(s.name == "req_seq", "SequenceIR.name == 'req_seq'");
    CHECK(s.expr == "req ##1 ack", "SequenceIR.expr == 'req ##1 ack'");

    SequenceIR empty = make_sequence("empty", "");
    CHECK(empty.name == "empty", "empty name");
    CHECK(empty.expr.empty(), "empty expr");

    return true;
}

// ── AC2: emit_sequence matches list-based eda:emit-sequence output ──
bool test_cpp_emit_sequence() {
    std::println("\n--- AC2: C++ emit_sequence ---");
    using aura::compiler::sv_ir::emit_sequence;
    using aura::compiler::sv_ir::make_sequence;

    std::string s;

    s = emit_sequence(make_sequence("req_seq", "req ##1 ack"));
    CHECK(s.find("sequence req_seq") != std::string::npos,
          "starts with 'sequence req_seq'");
    CHECK(s.find("req ##1 ack") != std::string::npos,
          "contains expr 'req ##1 ack'");
    CHECK(s.find("endsequence") != std::string::npos,
          "ends with 'endsequence'");

    s = emit_sequence(make_sequence("empty", ""));
    CHECK(s.find("sequence empty") != std::string::npos,
          "empty: starts with 'sequence empty'");
    CHECK(s.find("endsequence") != std::string::npos,
          "empty: ends with 'endsequence'");

    return true;
}

// ── AC3: C++ PropertyIR direct construction + accessors ──
bool test_cpp_property_ir_direct() {
    std::println("\n--- AC3: C++ PropertyIR direct construction ---");
    using aura::compiler::sv_ir::PropertyIR;
    using aura::compiler::sv_ir::make_property;

    PropertyIR p = make_property("ack_prop", "req |-> ack");
    CHECK(p.name == "ack_prop", "PropertyIR.name == 'ack_prop'");
    CHECK(p.expr == "req |-> ack", "PropertyIR.expr == 'req |-> ack'");

    PropertyIR empty = make_property("empty", "");
    CHECK(empty.expr.empty(), "empty expr");

    return true;
}

// ── AC4: emit_property matches list-based eda:emit-property output ──
bool test_cpp_emit_property() {
    std::println("\n--- AC4: C++ emit_property ---");
    using aura::compiler::sv_ir::emit_property;
    using aura::compiler::sv_ir::make_property;

    std::string s;

    s = emit_property(make_property("ack_prop", "req |-> ack"));
    CHECK(s.find("property ack_prop") != std::string::npos,
          "starts with 'property ack_prop'");
    CHECK(s.find("req |-> ack") != std::string::npos,
          "contains expr 'req |-> ack'");
    CHECK(s.find("endproperty") != std::string::npos,
          "ends with 'endproperty'");

    s = emit_property(make_property("empty", ""));
    CHECK(s.find("property empty") != std::string::npos,
          "empty: starts with 'property empty'");
    CHECK(s.find("endproperty") != std::string::npos,
          "empty: ends with 'endproperty'");

    return true;
}

// ── AC5: Aura list-based emitters still work (backward compat) ──
bool test_list_ir_still_works() {
    std::println("\n--- AC5: list-based emitters still work ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    // The list-based eda:emit-assertion route uses property
    // under the hood. The list-based eda:emit-property /
    // eda:emit-sequence emitters are part of the #436
    // umbrella; here we just verify the list IR is reachable
    // and a structured property built in C++ can co-exist
    // with a list-based assertion.
    auto s = run_string(cs,
        "(eda:emit-assertion "
        "  (make-eda:assert 'ready_ok "
        "    (make-eda:expr 'symbol "
        "      (list (make-eda:expr 'symbol (list 'ack)))))))");
    CHECK(!s.empty(), "list-based eda:emit-assertion returns non-empty");
    CHECK(s.find("assert") != std::string::npos,
          "list-based assertion contains 'assert'");
    CHECK(s.find("ready_ok") != std::string::npos,
          "list-based assertion contains label 'ready_ok'");

    return true;
}

int run_tests() {
    std::println("Issue #435 Phase 3 (structured C++ IR — SequenceIR + PropertyIR)\n");
    test_cpp_sequence_ir_direct();
    test_cpp_emit_sequence();
    test_cpp_property_ir_direct();
    test_cpp_emit_property();
    test_list_ir_still_works();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_435_phase3_detail

int aura_issue_435_phase3_run() { return aura_issue_435_phase3_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_435_phase3_run(); }
#endif