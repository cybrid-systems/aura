// @category: integration
// @reason: SV-idiomatic always block variants (always_comb /
//          always_ff) — extends the existing eda:always IR with a
//          kind field, adds convenience constructors, and routes
//          the Verilog emitter by kind. (Issue #436 Phase 1:
//          core SV constructs.)

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

namespace aura_issue_436_detail {

// Run a snippet and return the integer result. Returns -1 on failure / non-int.
static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// Run a snippet and return the string result.
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

// Run a snippet and return the keyword/symbol result as a string.
static std::string run_symbol_str(aura::compiler::CompilerService& cs, const std::string& src) {
    return run_string(cs, std::string("(eda:name-str ") + src + ")");
}

// ── AC1: make-eda:always-comb + eda:always-kind ──
bool test_always_comb_constructor() {
    std::println("\n--- AC1: make-eda:always-comb + eda:always-kind ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a (make-eda:always-comb '() (list)))")) {
        ++g_failed;
        return false;
    }

    // Predicate
    auto is_always = run_int(cs,
        "(if (eda:always? a) 1 0)");
    CHECK(is_always == 1, "(eda:always? a) returns #t");

    // Kind accessor
    auto kind = run_symbol_str(cs, "(eda:always-kind a)");
    CHECK(kind == "always_comb", "(eda:always-kind a) == 'always_comb");

    // Sensitivity is empty
    auto ss_len = run_int(cs, "(length (eda:always-sensitivity a))");
    CHECK(ss_len == 0, "(eda:always-sensitivity a) is empty list");

    // Body is empty
    auto body_len = run_int(cs, "(length (eda:always-body a))");
    CHECK(body_len == 0, "(eda:always-body a) is empty list");

    return true;
}

// ── AC2: make-eda:always-ff with edge sensitivity ──
bool test_always_ff_constructor() {
    std::println("\n--- AC2: make-eda:always-ff + edge sensitivity ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a "
                 "  (make-eda:always-ff "
                 "    (list (make-eda:sensitivity 'posedge 'clk)) "
                 "    (list (make-eda:assign "
                 "           (make-eda:expr 'symbol (list 'q-next)) "
                 "           (make-eda:expr 'symbol (list 'd))))))")) {
        ++g_failed;
        return false;
    }

    auto kind = run_symbol_str(cs, "(eda:always-kind a)");
    CHECK(kind == "always_ff", "(eda:always-kind a) == 'always_ff");

    auto ss_len = run_int(cs, "(length (eda:always-sensitivity a))");
    CHECK(ss_len == 1, "(eda:always-sensitivity a) has length 1 (posedge clk)");

    auto body_len = run_int(cs, "(length (eda:always-body a))");
    CHECK(body_len == 1, "(eda:always-body a) has length 1 (q-next <= d)");

    return true;
}

// ── AC3: backward compat — make-eda:always defaults kind to 'always ──
bool test_always_legacy_default_kind() {
    std::println("\n--- AC3: legacy make-eda:always defaults kind to 'always ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // Use the 2-arg form — should still work, defaulting kind='always.
    if (!cs.eval("(define a (make-eda:always '(list) (list)))")) {
        ++g_failed;
        return false;
    }
    auto kind = run_symbol_str(cs, "(eda:always-kind a)");
    CHECK(kind == "always", "(make-eda:always) defaults kind to 'always");

    // 3-arg form (make-eda:always3) takes explicit kind.
    if (!cs.eval("(define a3 (make-eda:always3 'always_ff '() '()))")) {
        ++g_failed;
        return false;
    }
    auto kind3 = run_symbol_str(cs, "(eda:always-kind a3)");
    CHECK(kind3 == "always_ff", "(make-eda:always3 'always_ff ...) sets kind");

    return true;
}

// ── AC4: emit always_comb — no @(...) when sensitivity is empty ──
bool test_emit_always_comb() {
    std::println("\n--- AC4: eda:emit always_comb (no @(*) when sens empty) ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a "
                 "  (make-eda:always-comb '() (list (make-eda:assign "
                 "           (make-eda:expr 'symbol (list 'y)) "
                 "           (make-eda:expr 'symbol (list 'a)))))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-always a)");
    // Expected: "always_comb begin\n  assign y = a;\nend\n"
    bool has_comb_kw = (s.find("always_comb") != std::string::npos);
    CHECK(has_comb_kw, "emitted SV contains 'always_comb'");

    // The plain "always_comb" should appear WITHOUT "@(" immediately
    // following when sensitivity is empty (SV implicitly @(*)s).
    bool no_at_paren = (s.find("always_comb @(") == std::string::npos);
    CHECK(no_at_paren, "empty-sensitivity always_comb does NOT emit '@('");

    bool has_begin = (s.find("begin") != std::string::npos);
    CHECK(has_begin, "emitted SV contains 'begin'");

    bool has_end = (s.find("end\n") != std::string::npos || s.find("end") != std::string::npos);
    CHECK(has_end, "emitted SV contains 'end'");

    bool has_assign = (s.find("assign y = a") != std::string::npos);
    CHECK(has_assign, "emitted SV contains body assign 'y = a'");

    return true;
}

// ── AC5: emit always_ff — emits @(...) with edge sensitivity ──
bool test_emit_always_ff() {
    std::println("\n--- AC5: eda:emit always_ff with posedge clk ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a "
                 "  (make-eda:always-ff "
                 "    (list (make-eda:sensitivity 'posedge 'clk)) "
                 "    (list (make-eda:assign "
                 "           (make-eda:expr 'symbol (list 'q)) "
                 "           (make-eda:expr 'symbol (list 'd)))))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-always a)");
    // Expected: "always_ff @(posedge clk) begin\n  assign q = d;\nend\n"
    bool has_ff_kw = (s.find("always_ff") != std::string::npos);
    CHECK(has_ff_kw, "emitted SV contains 'always_ff'");

    bool has_posedge = (s.find("posedge clk") != std::string::npos);
    CHECK(has_posedge, "emitted SV contains 'posedge clk'");

    bool has_begin_end = (s.find("begin") != std::string::npos) &&
                         (s.find("end") != std::string::npos);
    CHECK(has_begin_end, "emitted SV contains 'begin' and 'end'");

    return true;
}

// ── AC6: emit legacy always — keeps old "always @(...)" form ──
bool test_emit_legacy_always() {
    std::println("\n--- AC6: legacy 'always still emits old form ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a "
                 "  (make-eda:always "
                 "    (list (make-eda:sensitivity 'posedge 'clk)) "
                 "    (list)))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-always a)");
    // Should still emit "always @(" form (backward compat).
    bool has_legacy = (s.find("always @(") != std::string::npos);
    CHECK(has_legacy, "legacy 'always still emits 'always @(' form (backward compat)");

    bool has_posedge = (s.find("posedge clk") != std::string::npos);
    CHECK(has_posedge, "legacy always carries posedge clk sensitivity");

    // Should NOT have always_comb or always_ff in the output.
    bool no_comb = (s.find("always_comb") == std::string::npos);
    bool no_ff = (s.find("always_ff") == std::string::npos);
    CHECK(no_comb && no_ff, "legacy always does NOT emit always_comb/always_ff");

    return true;
}

// ── AC7: eda:emit-verilog + module integration — always_comb in a module ──
bool test_module_with_always_comb() {
    std::println("\n--- AC7: module with always_comb (full emit-verilog) ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define m "
                 "  (make-eda:module 'mux2 "
                 "    (list (make-eda:port 'a 'input 1) "
                 "          (make-eda:port 'b 'input 1) "
                 "          (make-eda:port 'sel 'input 1) "
                 "          (make-eda:port 'y 'output 1)) "
                 "    (list "
                 "      (make-eda:always-comb '() "
                 "        (list "
                 "          (make-eda:assign "
                 "            (make-eda:expr 'symbol (list 'y)) "
                 "            (make-eda:expr '+ "
                 "              (list "
                 "                (make-eda:expr '& "
                 "                  (list "
                 "                    (make-eda:expr 'symbol (list 'sel)) "
                 "                    (make-eda:expr 'symbol (list 'a)))) "
                 "                (make-eda:expr '& "
                 "                  (list "
                 "                    (make-eda:expr 'symbol "
                 "                      (list 'sel)) "
                 "                    (make-eda:expr 'symbol (list 'b)))))))))))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-verilog m)");
    bool has_module = (s.find("module mux2") != std::string::npos);
    CHECK(has_module, "module emit contains 'module mux2'");

    bool has_comb = (s.find("always_comb") != std::string::npos);
    CHECK(has_comb, "module emit contains 'always_comb'");

    bool has_endmodule = (s.find("endmodule") != std::string::npos);
    CHECK(has_endmodule, "module emit ends with 'endmodule'");

    // Body should NOT have plain 'always' (since we used always_comb).
    bool no_legacy = (s.find("always @(") == std::string::npos);
    CHECK(no_legacy, "module with always_comb does NOT also emit legacy 'always @('");

    return true;
}

int run_tests() {
    std::println("Issue #436 Phase 1 (always_comb / always_ff SV variants)\n");
    test_always_comb_constructor();
    test_always_ff_constructor();
    test_always_legacy_default_kind();
    test_emit_always_comb();
    test_emit_always_ff();
    test_emit_legacy_always();
    test_module_with_always_comb();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_436_detail

int aura_issue_436_run() { return aura_issue_436_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_436_run(); }
#endif