// @category: integration
// @reason: SV-idiomatic always_latch + SVA properties
//          (assert / assume / cover property) (Issue #436
//          Phase 3: latch + SVA foundation).


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_436_phase3_detail {

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

// ── AC1: always_latch constructor + accessors ──
bool test_always_latch_constructor() {
    std::println("\n--- AC1: make-eda:always-latch + accessors ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a (make-eda:always-latch "
                 "  (list (make-eda:sensitivity 'level 'en)) "
                 "  (list)))")) {
        ++g_failed;
        return false;
    }

    // Predicate
    auto is_always = run_int(cs, "(if (eda:always? a) 1 0)");
    CHECK(is_always == 1, "(eda:always? a) returns #t");

    // Kind accessor
    auto kind = run_symbol_str(cs, "(eda:always-kind a)");
    CHECK(kind == "always_latch", "(eda:always-kind a) == 'always_latch");

    // Sensitivity preserved
    auto ss_len = run_int(cs, "(length (eda:always-sensitivity a))");
    CHECK(ss_len == 1, "sensitivity list has length 1");

    return true;
}

// ── AC2: emit always_latch with empty sens (omits @()) ──
bool test_emit_always_latch_empty_sens() {
    std::println("\n--- AC2: emit always_latch, empty sens ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a (make-eda:always-latch '() "
                 "  (list (make-eda:assign "
                 "          (make-eda:expr 'symbol (list 'q)) "
                 "          (make-eda:expr 'symbol (list 'd)))))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-always a)");
    bool has_latch = (s.find("always_latch") != std::string::npos);
    CHECK(has_latch, "emitted SV contains 'always_latch'");

    bool no_at = (s.find("always_latch @(") == std::string::npos);
    CHECK(no_at, "always_latch with empty sens does NOT emit '@('");

    bool has_begin = (s.find("begin") != std::string::npos);
    bool has_end = (s.find("end") != std::string::npos);
    CHECK(has_begin && has_end, "emitted SV has begin/end");

    bool has_assign = (s.find("assign q = d") != std::string::npos);
    CHECK(has_assign, "emitted SV has body assign 'q = d'");

    return true;
}

// ── AC3: eda:property + eda:assertion constructors + accessors ──
bool test_property_and_assertion_constructors() {
    std::println("\n--- AC3: make-eda:assert + accessors ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a (make-eda:assert 'req_then_ack "
                 "  (make-eda:expr 'symbol (list 'ack)))))")) {
        ++g_failed;
        return false;
    }

    auto is_assertion = run_int(cs, "(if (eda:assertion? a) 1 0)");
    CHECK(is_assertion == 1, "(eda:assertion? a) returns #t");

    auto kind = run_symbol_str(cs, "(eda:assertion-kind a)");
    CHECK(kind == "assert", "(eda:assertion-kind a) == 'assert");

    auto name = run_symbol_str(cs, "(eda:assertion-name a)");
    CHECK(name == "req_then_ack", "(eda:assertion-name a) == 'req_then_ack'");

    // Property is auto-built
    auto is_prop = run_int(cs, "(if (eda:property? (eda:assertion-property a)) 1 0)");
    CHECK(is_prop == 1, "(eda:assertion-property a) returns an eda:property");

    auto prop_name = run_symbol_str(cs, "(eda:property-name (eda:assertion-property a))");
    CHECK(prop_name == "req_then_ack", "auto-built property has the same name as the assertion");

    return true;
}

// ── AC4: eda:emit-assertion — assert / assume / cover ──
bool test_emit_assertion_kinds() {
    std::println("\n--- AC4: emit-assertion for assert / assume / cover ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define expr (make-eda:expr 'symbol (list 'ready)))")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define prop (make-eda:property 'shared expr))")) {
        ++g_failed;
        return false;
    }

    // assert
    if (!cs.eval("(define a1 (make-eda:assertion 'assert 'ready_ok prop))")) {
        ++g_failed;
        return false;
    }
    auto s1 = run_string(cs, "(eda:emit-assertion a1)");
    bool has_assert = (s1.find("assert property (ready)") != std::string::npos);
    CHECK(has_assert, "assert emits 'assert property (ready);'");

    // assume
    if (!cs.eval("(define a2 (make-eda:assertion 'assume 'req_held prop))")) {
        ++g_failed;
        return false;
    }
    auto s2 = run_string(cs, "(eda:emit-assertion a2)");
    bool has_assume = (s2.find("assume property (ready)") != std::string::npos);
    CHECK(has_assume, "assume emits 'assume property (ready);'");

    // cover
    if (!cs.eval("(define a3 (make-eda:assertion 'cover 'burst_done prop))")) {
        ++g_failed;
        return false;
    }
    auto s3 = run_string(cs, "(eda:emit-assertion a3)");
    bool has_cover = (s3.find("cover property (ready)") != std::string::npos);
    CHECK(has_cover, "cover emits 'cover property (ready);'");

    // All three should contain the name as a comment
    bool name_in_1 = (s1.find("ready_ok") != std::string::npos);
    bool name_in_2 = (s2.find("req_held") != std::string::npos);
    bool name_in_3 = (s3.find("burst_done") != std::string::npos);
    CHECK(name_in_1 && name_in_2 && name_in_3,
          "all three emit the assertion name as a trailing comment");

    return true;
}

// ── AC5: assertions in module body — full emit-verilog ──
bool test_assertion_in_module() {
    std::println("\n--- AC5: assertions in module body (full emit-verilog) ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define m "
                 "  (make-eda:module 'handshake "
                 "    (list (make-eda:port 'req 'input 1) "
                 "          (make-eda:port 'ack 'output 1)) "
                 "    (list "
                 "      (make-eda:always-ff "
                 "        (list (make-eda:sensitivity 'posedge 'clk)) "
                 "        (list)) "
                 "      (make-eda:assert 'req_ack "
                 "        (make-eda:expr 'symbol (list 'ack))) "
                 "      (make-eda:assume 'req_held "
                 "        (make-eda:expr 'symbol (list 'req))) "
                 "      (make-eda:cover 'burst "
                 "        (make-eda:expr 'symbol (list 'req)))))))")) {
        ++g_failed;
        return false;
    }

    auto s = run_string(cs, "(eda:emit-verilog m)");

    // Module + always_ff still emit
    bool has_module = (s.find("module handshake") != std::string::npos);
    CHECK(has_module, "module emits 'module handshake'");
    bool has_ff = (s.find("always_ff") != std::string::npos);
    CHECK(has_ff, "module emits 'always_ff'");

    // All three assertions in output
    bool has_assert = (s.find("assert property (ack)") != std::string::npos);
    bool has_assume = (s.find("assume property (req)") != std::string::npos);
    bool has_cover = (s.find("cover property (req)") != std::string::npos);
    CHECK(has_assert, "module emits 'assert property (ack);'");
    CHECK(has_assume, "module emits 'assume property (req);'");
    CHECK(has_cover, "module emits 'cover property (req);'");

    // Module ends with endmodule
    bool has_endmodule = (s.find("endmodule") != std::string::npos);
    CHECK(has_endmodule, "module still ends with 'endmodule'");

    return true;
}

// ── AC6: eda:display-assertion + dispatcher ──
bool test_display_assertion() {
    std::println("\n--- AC6: eda:display-assertion + eda:display dispatch ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a (make-eda:cover 'handshake_done "
                 "  (make-eda:expr 'symbol (list 'done)))))")) {
        ++g_failed;
        return false;
    }

    // eda:display dispatches to eda:display-assertion
    auto r1 = cs.eval("(eda:display a)");
    CHECK(r1.has_value(), "(eda:display assertion) dispatches without crash");

    // Negative: an always is not an assertion
    if (!cs.eval("(define al (make-eda:always-comb '() (list)))")) {
        ++g_failed;
        return false;
    }
    auto not_a = run_int(cs, "(if (eda:assertion? al) 1 0)");
    CHECK(not_a == 0, "(eda:assertion? <always>) returns #f");

    return true;
}

// ── AC7: all 4 always kinds (always / comb / ff / latch) coexist in same module ──
bool test_all_four_kinds_coexist() {
    std::println("\n--- AC7: all 4 always kinds in one module ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define m "
                 "  (make-eda:module 'mixed "
                 "    (list (make-eda:port 'clk 'input 1) "
                 "          (make-eda:port 'en 'input 1) "
                 "          (make-eda:port 'a 'input 1) "
                 "          (make-eda:port 'q 'output 1)) "
                 "    (list "
                 "      (make-eda:always '() (list)) "
                 "      (make-eda:always-comb '() (list)) "
                 "      (make-eda:always-ff "
                 "        (list (make-eda:sensitivity 'posedge 'clk)) (list)) "
                 "      (make-eda:always-latch '() (list))))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-verilog m)");

    // All 4 should appear
    bool has_legacy = (s.find("always @(") != std::string::npos);
    bool has_comb = (s.find("always_comb") != std::string::npos);
    bool has_ff = (s.find("always_ff") != std::string::npos);
    bool has_latch = (s.find("always_latch") != std::string::npos);
    CHECK(has_legacy, "legacy 'always @(' appears");
    CHECK(has_comb, "always_comb appears");
    CHECK(has_ff, "always_ff appears");
    CHECK(has_latch, "always_latch appears");

    return true;
}

int run_tests() {
    std::println("Issue #436 Phase 3 (always_latch + SVA properties)\n");
    test_always_latch_constructor();
    test_emit_always_latch_empty_sens();
    test_property_and_assertion_constructors();
    test_emit_assertion_kinds();
    test_assertion_in_module();
    test_display_assertion();
    test_all_four_kinds_coexist();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_436_phase3_detail

int aura_issue_436_phase3_run() {
    return aura_issue_436_phase3_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_436_phase3_run();
}
#endif