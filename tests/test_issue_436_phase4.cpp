// @category: integration
// @reason: SV declaration kinds (wire / logic / reg / bit) for
//          module body signals (Issue #436 Phase 4: SV
//          declaration coverage).

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

namespace aura_issue_436_phase4_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

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

static std::string run_symbol_str(aura::compiler::CompilerService& cs, const std::string& src) {
    return run_string(cs, std::string("(eda:name-str ") + src + ")");
}

// ── AC1: 4 kinds of constructors all build the right IR ──
bool test_four_kinds_constructors() {
    std::println("\n--- AC1: make-eda:wire / logic / reg / bit ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    if (!cs.eval("(define w (make-eda:wire 'data 8))")) {
        ++g_failed;
        return false;
    }
    auto kind_w = run_symbol_str(cs, "(eda:wire-kind w)");
    CHECK(kind_w == "wire", "make-eda:wire defaults kind to 'wire");

    if (!cs.eval("(define l (make-eda:logic 'q 1))")) {
        ++g_failed;
        return false;
    }
    auto kind_l = run_symbol_str(cs, "(eda:wire-kind l)");
    CHECK(kind_l == "logic", "make-eda:logic sets kind to 'logic");

    if (!cs.eval("(define r (make-eda:reg 'q 8))")) {
        ++g_failed;
        return false;
    }
    auto kind_r = run_symbol_str(cs, "(eda:wire-kind r)");
    CHECK(kind_r == "reg", "make-eda:reg sets kind to 'reg");

    if (!cs.eval("(define b (make-eda:bit 'flag 1))")) {
        ++g_failed;
        return false;
    }
    auto kind_b = run_symbol_str(cs, "(eda:wire-kind b)");
    CHECK(kind_b == "bit", "make-eda:bit sets kind to 'bit");

    return true;
}

// ── AC2: eda:emit-wire emits the right keyword for each kind ──
bool test_emit_wire_keywords() {
    std::println("\n--- AC2: eda:emit-wire per-kind keyword ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define w (make-eda:wire 'd 8))")) {
        ++g_failed;
        return false;
    }
    auto sw = run_string(cs, "(eda:emit-wire w)");
    CHECK(sw == "wire [8-1:0] d;", std::string("wire emit = \"") + sw + "\"");

    if (!cs.eval("(define l (make-eda:logic 'q 1))")) {
        ++g_failed;
        return false;
    }
    auto sl = run_string(cs, "(eda:emit-wire l)");
    CHECK(sl == "logic q;", std::string("logic emit = \"") + sl + "\"");

    if (!cs.eval("(define r (make-eda:reg 'q 8))")) {
        ++g_failed;
        return false;
    }
    auto sr = run_string(cs, "(eda:emit-wire r)");
    CHECK(sr == "reg [8-1:0] q;", std::string("reg emit = \"") + sr + "\"");

    if (!cs.eval("(define b (make-eda:bit 'f 1))")) {
        ++g_failed;
        return false;
    }
    auto sb = run_string(cs, "(eda:emit-wire b)");
    CHECK(sb == "bit f;", std::string("bit emit = \"") + sb + "\"");

    return true;
}

// ── AC3: make-eda:wire3 explicit kind ──
bool test_wire3_explicit_kind() {
    std::println("\n--- AC3: make-eda:wire3 with explicit kind ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // Explicit 3-arg form
    if (!cs.eval("(define w3 (make-eda:wire3 'q 4 'logic))")) {
        ++g_failed;
        return false;
    }
    auto kind = run_symbol_str(cs, "(eda:wire-kind w3)");
    CHECK(kind == "logic", "make-eda:wire3 sets explicit kind");

    auto name = run_symbol_str(cs, "(eda:wire-name w3)");
    CHECK(name == "q", "name preserved");

    auto width = run_int(cs, "(eda:wire-width w3)");
    CHECK(width == 4, "width preserved");

    return true;
}

// ── AC4: backward compat — existing make-eda:wire 2-arg form still works ──
bool test_legacy_2arg_wire_compat() {
    std::println("\n--- AC4: legacy make-eda:wire 2-arg still works ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // Old 2-arg call shape — should still work, kind='wire.
    if (!cs.eval("(define w (make-eda:wire 'q 1))")) {
        ++g_failed;
        return false;
    }
    auto kind = run_symbol_str(cs, "(eda:wire-kind w)");
    CHECK(kind == "wire", "legacy 2-arg form defaults kind to 'wire");

    auto sw = run_string(cs, "(eda:emit-wire w)");
    CHECK(sw == "wire q;", std::string("legacy emit = \"") + sw + "\"");

    return true;
}

// ── AC5: mixed module with all 4 kinds ──
bool test_mixed_kinds_in_module() {
    std::println("\n--- AC5: all 4 kinds in one module body ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define m "
                 "  (make-eda:module 'mix_decl "
                 "    (list (make-eda:port 'clk 'input 1)) "
                 "    (list "
                 "      (make-eda:wire 'data_w 8) "
                 "      (make-eda:logic 'q_l 1) "
                 "      (make-eda:reg 'acc_r 32) "
                 "      (make-eda:bit 'flag_b 1))))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-verilog m)");

    // All 4 keywords present
    bool has_w = (s.find("wire [8-1:0] data_w;") != std::string::npos);
    bool has_l = (s.find("logic q_l;") != std::string::npos);
    bool has_r = (s.find("reg [32-1:0] acc_r;") != std::string::npos);
    bool has_b = (s.find("bit flag_b;") != std::string::npos);
    CHECK(has_w, "module contains 'wire [8-1:0] data_w;'");
    CHECK(has_l, "module contains 'logic q_l;'");
    CHECK(has_r, "module contains 'reg [32-1:0] acc_r;'");
    CHECK(has_b, "module contains 'bit flag_b;'");

    return true;
}

// ── AC6: eda:display-wire shows the kind ──
bool test_display_wire_uses_kind() {
    std::println("\n--- AC6: eda:display-wire shows the kind ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define w (make-eda:logic 'q 8))")) {
        ++g_failed;
        return false;
    }
    // eda:display dispatches to eda:display-wire. We can't easily capture
    // stdout, but eda:display-wire must not crash. The kind is reflected
    // in the output (verified via emit).
    auto sw = run_string(cs, "(eda:emit-wire w)");
    bool has_logic_kw = (sw.substr(0, 5) == "logic");
    CHECK(has_logic_kw, std::string("wire emit starts with 'logic' (got: \"") + sw + "\")");

    auto r = cs.eval("(eda:display w)");
    CHECK(r.has_value(), "(eda:display wire) does not crash");

    return true;
}

int run_tests() {
    std::println("Issue #436 Phase 4 (logic / reg / bit declarations)\n");
    test_four_kinds_constructors();
    test_emit_wire_keywords();
    test_wire3_explicit_kind();
    test_legacy_2arg_wire_compat();
    test_mixed_kinds_in_module();
    test_display_wire_uses_kind();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_436_phase4_detail

int aura_issue_436_phase4_run() { return aura_issue_436_phase4_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_436_phase4_run(); }
#endif