// @category: integration
// @reason: SV generate blocks (genvar / generate-for /
//          generate-if) (Issue #436 Phase 6: SV generate
//          constructs).


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_436_phase6_detail {

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

// ── AC1: genvar constructor + accessors + emit ──
bool test_genvar_constructor() {
    std::println("\n--- AC1: make-eda:genvar + accessors + emit ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define g (make-eda:genvar 'i 0))")) {
        ++g_failed;
        return false;
    }
    // DEBUG: probe each piece
    auto raw = run_string(cs,
        "(string-append \"raw=\" (eda:name-str (eda:genvar-name-debug g)) "
        "\" len=\" (number->string (eda:genvar-init g)))");
    std::println("  DEBUG g raw=[{}]", raw);
    auto raw2 = run_string(cs, "(eda:name-str (eda:genvar-name-debug g))");
    std::println("  DEBUG genvar-name-debug = [{}]", raw2);
    // Try directly probing what's in the env
    auto probe = run_string(cs,
        "(let ((probe-result (eda:genvar-name-debug g))) "
        "  (string-append \"probe:[\" (eda:name-str probe-result) \"]\"))");
    std::println("  DEBUG probe = [{}]", probe);
    // Direct inlined car/cdr test
    auto inline_test = run_string(cs, "(car (cdr g))");
    std::println("  DEBUG inline (car (cdr g)) = [{}] (len {})", inline_test, inline_test.size());
    auto via_name_str = run_string(cs, "(eda:name-str (car (cdr g)))");
    std::println("  DEBUG via name-str = [{}]", via_name_str);
    auto via_genvar = run_string(cs, "(eda:name-str (eda:genvar-name g))");
    std::println("  DEBUG via eda:genvar-name = [{}]", via_genvar);
    auto via_genvar_debug = run_string(cs, "(eda:name-str (eda:genvar-name-debug g))");
    std::println("  DEBUG via eda:genvar-name-debug = [{}]", via_genvar_debug);
    // Direct test of name-str
    auto ns_i = run_string(cs, "(eda:name-str 'i)");
    std::println("  DEBUG (eda:name-str 'i) = [{}]", ns_i);
    auto ns_empty = run_string(cs, "(eda:name-str '())");
    std::println("  DEBUG (eda:name-str '()) = [{}]", ns_empty);
    auto ns_symbol = run_string(cs, "(eda:name-str (quote i))");
    std::println("  DEBUG (eda:name-str (quote i)) = [{}]", ns_symbol);
    auto sa_test = run_string(cs, "(string-append \"\" 'i)");
    std::println("  DEBUG (string-append \"\" 'i) = [{}]", sa_test);
    // Test on existing types
    auto ns_ac = run_string(cs, "(eda:name-str 'always_comb)");
    std::println("  DEBUG (eda:name-str 'always_comb) = [{}]", ns_ac);
    auto sa_ac = run_string(cs, "(string-append \"\" 'always_comb)");
    std::println("  DEBUG (string-append \"\" 'always_comb) = [{}]", sa_ac);
    auto is_g = run_int(cs, "(if (eda:genvar? g) 1 0)");
    CHECK(is_g == 1, "(eda:genvar? g) returns #t");

    auto name = run_symbol_str(cs, "(eda:genvar-name g)");
    CHECK(name == "i", "genvar name == 'i'");

    auto init = run_int(cs, "(eda:genvar-init g)");
    CHECK(init == 0, "genvar init == 0");

    auto s = run_string(cs, "(eda:emit-genvar g)");
    std::println("  DEBUG emit-genvar result = [{}]", s);
    CHECK(s == "genvar i = 0;",
          std::string("genvar emit = \"") + s + "\"");

    // Non-zero init
    if (!cs.eval("(define g2 (make-eda:genvar 'k 3))")) {
        ++g_failed;
        return false;
    }
    auto s2 = run_string(cs, "(eda:emit-genvar g2)");
    CHECK(s2 == "genvar k = 3;",
          std::string("genvar k emit = \"") + s2 + "\"");

    return true;
}

// ── AC2: generate-for with wire body ──
bool test_generate_for_with_body() {
    std::println("\n--- AC2: generate-for with wire body ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // genvar i; for (i=0; i<4; i++) begin wire [7:0] inst_i; end
    if (!cs.eval("(define gf "
                 "  (make-eda:generate-for 'i 0 4 "
                 "    (list (make-eda:wire 'inst 8))))")) {
        ++g_failed;
        return false;
    }

    auto is_gf = run_int(cs, "(if (eda:generate-for? gf) 1 0)");
    CHECK(is_gf == 1, "(eda:generate-for? gf) returns #t");

    auto name = run_symbol_str(cs, "(eda:generate-for-name gf)");
    CHECK(name == "i", "for-loop name == 'i'");

    auto init = run_int(cs, "(eda:generate-for-init gf)");
    CHECK(init == 0, "for-loop init == 0");

    auto max = run_int(cs, "(eda:generate-for-max gf)");
    CHECK(max == 4, "for-loop max == 4");

    auto body_len = run_int(cs, "(length (eda:generate-for-body gf))");
    CHECK(body_len == 1, "for-loop body has 1 item");

    auto s = run_string(cs, "(eda:emit-generate-for gf)");
    bool has_for_kw = (s.find("for (genvar i = 0; i < 4;") != std::string::npos);
    CHECK(has_for_kw, "for-loop emit has 'for (genvar i = 0; i < 4;'");

    bool has_begin = (s.find("begin") != std::string::npos);
    bool has_end = (s.find("end") != std::string::npos);
    CHECK(has_begin && has_end, "for-loop has begin/end");

    bool has_wire = (s.find("wire [8-1:0] inst;") != std::string::npos);
    CHECK(has_wire, "for-loop body has 'wire [8-1:0] inst;'");

    return true;
}

// ── AC3: generate-if with then/else bodies ──
bool test_generate_if() {
    std::println("\n--- AC3: generate-if with then/else ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define gi "
                 "  (make-eda:generate-if 'USE_BIG "
                 "    (list (make-eda:wire 'big_q 32)) "
                 "    (list (make-eda:wire 'small_q 8))))")) {
        ++g_failed;
        return false;
    }

    auto is_gi = run_int(cs, "(if (eda:generate-if? gi) 1 0)");
    CHECK(is_gi == 1, "(eda:generate-if? gi) returns #t");

    auto cond = run_symbol_str(cs, "(eda:generate-if-cond gi)");
    CHECK(cond == "USE_BIG", "if-cond == 'USE_BIG'");

    auto then_len = run_int(cs, "(length (eda:generate-if-then gi))");
    CHECK(then_len == 1, "then-body has 1 item");

    auto else_len = run_int(cs, "(length (eda:generate-if-else gi))");
    CHECK(else_len == 1, "else-body has 1 item");

    auto s = run_string(cs, "(eda:emit-generate-if gi)");
    bool has_if_kw = (s.find("if (USE_BIG) begin") != std::string::npos);
    CHECK(has_if_kw, "if-emit starts with 'if (USE_BIG) begin'");

    bool has_else = (s.find("else begin") != std::string::npos);
    CHECK(has_else, "if-emit has 'else begin'");

    bool has_big = (s.find("wire [32-1:0] big_q;") != std::string::npos);
    CHECK(has_big, "if-then has 'wire [32-1:0] big_q;'");

    bool has_small = (s.find("wire [8-1:0] small_q;") != std::string::npos);
    CHECK(has_small, "if-else has 'wire [8-1:0] small_q;'");

    return true;
}

// ── AC4: generate-if with no else branch (empty else) ──
bool test_generate_if_no_else() {
    std::println("\n--- AC4: generate-if with no else branch ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define gi "
                 "  (make-eda:generate-if 'HAS_FIFO "
                 "    (list (make-eda:logic 'fifo_full 1)) "
                 "    '())))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-generate-if gi)");
    bool has_if_kw = (s.find("if (HAS_FIFO) begin") != std::string::npos);
    CHECK(has_if_kw, "if-emit has 'if (HAS_FIFO) begin'");

    bool no_else = (s.find("else begin") == std::string::npos);
    CHECK(no_else, "no-else form does NOT emit 'else begin'");

    bool has_fifo = (s.find("logic fifo_full;") != std::string::npos);
    CHECK(has_fifo, "if-then has 'logic fifo_full;'");

    return true;
}

// ── AC5: generate in module body — full emit-verilog ──
bool test_generate_in_module() {
    std::println("\n--- AC5: genvar + generate-for + generate-if in module ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define m "
                 "  (make-eda:module 'arr4 "
                 "    (list (make-eda:port 'clk 'input 1) "
                 "          (make-eda:port 'out 'output 8)) "
                 "    (list "
                 "      (make-eda:genvar 'i 0) "
                 "      (make-eda:generate-for 'i 0 4 "
                 "        (list (make-eda:wire 'cell 8))) "
                 "      (make-eda:generate-if 'BIG_MODE "
                 "        (list (make-eda:wire 'wide 32)) "
                 "        '()) "
                 "      (make-eda:logic 'top_reg 8) "
                 "      (make-eda:always-ff "
                 "        (list (make-eda:sensitivity 'posedge 'clk)) "
                 "        (list))))))")) {
        ++g_failed;
        return false;
    }
    auto s = run_string(cs, "(eda:emit-verilog m)");

    bool has_module = (s.find("module arr4") != std::string::npos);
    CHECK(has_module, "module emits 'module arr4'");

    bool has_genvar = (s.find("genvar i = 0;") != std::string::npos);
    CHECK(has_genvar, "module contains 'genvar i = 0;'");

    bool has_for = (s.find("for (genvar i = 0; i < 4;") != std::string::npos);
    CHECK(has_for, "module contains generate-for");

    bool has_for_body = (s.find("wire [8-1:0] cell;") != std::string::npos);
    CHECK(has_for_body, "module contains for-body 'wire [8-1:0] cell;'");

    bool has_if = (s.find("if (BIG_MODE) begin") != std::string::npos);
    CHECK(has_if, "module contains 'if (BIG_MODE) begin'");

    bool has_top_reg = (s.find("logic [8-1:0] top_reg;") != std::string::npos);
    CHECK(has_top_reg, "module contains 'logic [8-1:0] top_reg;'");

    bool has_ff = (s.find("always_ff") != std::string::npos);
    CHECK(has_ff, "module contains 'always_ff'");

    bool has_endmodule = (s.find("endmodule") != std::string::npos);
    CHECK(has_endmodule, "module ends with 'endmodule'");

    return true;
}

// ── AC6: display dispatch for generate constructs ──
bool test_display_generate() {
    std::println("\n--- AC6: eda:display dispatch for generate ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define g (make-eda:genvar 'k 0))")) {
        ++g_failed;
        return false;
    }
    auto rg = cs.eval("(eda:display g)");
    CHECK(rg.has_value(), "(eda:display genvar) dispatches without crash");

    if (!cs.eval("(define gf (make-eda:generate-for 'i 0 3 '()))")) {
        ++g_failed;
        return false;
    }
    auto rgf = cs.eval("(eda:display gf)");
    CHECK(rgf.has_value(), "(eda:display generate-for) dispatches without crash");

    if (!cs.eval("(define gi (make-eda:generate-if 'COND '() '()))")) {
        ++g_failed;
        return false;
    }
    auto rgi = cs.eval("(eda:display gi)");
    CHECK(rgi.has_value(), "(eda:display generate-if) dispatches without crash");

    return true;
}

int run_tests() {
    std::println("Issue #436 Phase 6 (genvar / generate-for / generate-if)\n");
    test_genvar_constructor();
    test_generate_for_with_body();
    test_generate_if();
    test_generate_if_no_else();
    test_generate_in_module();
    test_display_generate();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_436_phase6_detail

int aura_issue_436_phase6_run() { return aura_issue_436_phase6_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_436_phase6_run(); }
#endif