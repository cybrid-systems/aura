// @category: integration
// @reason: auto-upgrade always @(*) / always @(posedge clk) to
//          always_comb / always_ff based on sensitivity context
//          (Issue #436 Phase 2: automatic idiom upgrade).


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_436_phase2_detail {

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

static std::string run_symbol_str(aura::compiler::CompilerService& cs, const std::string& src) {
    return run_string(cs, std::string("(eda:name-str ") + src + ")");
}

// ── AC1: detect-kind — empty sens → always_comb ──
bool test_detect_empty_sens_to_comb() {
    std::println("\n--- AC1: eda:always-detect-kind, empty sens → always_comb ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // Plain always with empty sens (i.e. always @(*) or always @())
    if (!cs.eval("(define a (make-eda:always '() (list)))")) {
        ++g_failed;
        return false;
    }
    auto kind = run_symbol_str(cs, "(eda:always-detect-kind a)");
    CHECK(kind == "always_comb",
          "empty sensitivity always @() detects as 'always_comb");

    return true;
}

// ── AC2: detect-kind — single posedge → always_ff ──
bool test_detect_single_posedge_to_ff() {
    std::println("\n--- AC2: eda:always-detect-kind, single posedge → always_ff ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a "
                 "  (make-eda:always "
                 "    (list (make-eda:sensitivity 'posedge 'clk)) "
                 "    (list)))")) {
        ++g_failed;
        return false;
    }
    auto kind = run_symbol_str(cs, "(eda:always-detect-kind a)");
    CHECK(kind == "always_ff",
          "single posedge clk detects as 'always_ff");

    // Same for negedge.
    if (!cs.eval("(define a2 "
                 "  (make-eda:always "
                 "    (list (make-eda:sensitivity 'negedge 'rst)) "
                 "    (list)))")) {
        ++g_failed;
        return false;
    }
    auto kind2 = run_symbol_str(cs, "(eda:always-detect-kind a2)");
    CHECK(kind2 == "always_ff",
          "single negedge rst also detects as 'always_ff");

    return true;
}

// ── AC3: detect-kind — multiple edges or level-sensitive → keep as 'always ──
bool test_detect_multi_or_level_keeps_always() {
    std::println("\n--- AC3: eda:always-detect-kind, multi/level → 'always ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // Two edges — async set/reset pattern, NOT a clean FF.
    if (!cs.eval("(define a-multi "
                 "  (make-eda:always "
                 "    (list (make-eda:sensitivity 'posedge 'clk) "
                 "          (make-eda:sensitivity 'negedge 'rst)) "
                 "    (list)))")) {
        ++g_failed;
        return false;
    }
    auto kind_multi = run_symbol_str(cs, "(eda:always-detect-kind a-multi)");
    CHECK(kind_multi == "always",
          "two-edge sens (posedge clk + negedge rst) keeps as 'always");

    // Level-sensitive: edge symbol is 'level (not posedge/negedge).
    if (!cs.eval("(define a-level "
                 "  (make-eda:always "
                 "    (list (make-eda:sensitivity 'level 'clk)) "
                 "    (list)))")) {
        ++g_failed;
        return false;
    }
    auto kind_level = run_symbol_str(cs, "(eda:always-detect-kind a-level)");
    CHECK(kind_level == "always",
          "level-sensitive (no edge) keeps as 'always");

    return true;
}

// ── AC4: upgrade-always — produces new always with detected kind, body preserved ──
bool test_upgrade_always_preserves_body() {
    std::println("\n--- AC4: eda:upgrade-always — body & sens preserved ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a "
                 "  (make-eda:always "
                 "    (list (make-eda:sensitivity 'posedge 'clk)) "
                 "    (list "
                 "      (make-eda:assign "
                 "        (make-eda:expr 'symbol (list 'q)) "
                 "        (make-eda:expr 'symbol (list 'd))))))")) {
        ++g_failed;
        return false;
    }
    // Upgrade — should reclassify to always_ff.
    if (!cs.eval("(define a-upgraded (eda:upgrade-always a))")) {
        ++g_failed;
        return false;
    }
    auto kind = run_symbol_str(cs, "(eda:always-kind a-upgraded)");
    CHECK(kind == "always_ff", "upgraded always has kind 'always_ff");

    // Sensitivity preserved.
    auto ss_len = run_int(cs, "(length (eda:always-sensitivity a-upgraded))");
    CHECK(ss_len == 1, "sensitivity list length preserved (1)");

    auto edge = run_symbol_str(cs,
        "(eda:sensitivity-edge (car (eda:always-sensitivity a-upgraded)))");
    CHECK(edge == "posedge", "sensitivity edge is still 'posedge");

    auto sig = run_symbol_str(cs,
        "(eda:sensitivity-signal (car (eda:always-sensitivity a-upgraded)))");
    CHECK(sig == "clk", "sensitivity signal is still 'clk");

    // Body preserved (1 assign).
    auto body_len = run_int(cs, "(length (eda:always-body a-upgraded))");
    CHECK(body_len == 1, "body length preserved (1 assign)");

    return true;
}

// ── AC5: upgrade-always is idempotent — upgrading an already-best always is a no-op ──
bool test_upgrade_idempotent() {
    std::println("\n--- AC5: eda:upgrade-always is idempotent ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // Already always_comb.
    if (!cs.eval("(define a (make-eda:always-comb '() (list)))")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define a-up (eda:upgrade-always a))")) {
        ++g_failed;
        return false;
    }
    auto kind = run_symbol_str(cs, "(eda:always-kind a-up)");
    CHECK(kind == "always_comb", "already-always_comb stays always_comb after upgrade");

    // Always_ff stays.
    if (!cs.eval("(define b (make-eda:always-ff "
                 "  (list (make-eda:sensitivity 'posedge 'clk)) (list)))")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define b-up (eda:upgrade-always b))")) {
        ++g_failed;
        return false;
    }
    auto kind_b = run_symbol_str(cs, "(eda:always-kind b-up)");
    CHECK(kind_b == "always_ff", "already-always_ff stays always_ff after upgrade");

    return true;
}

// ── AC6: upgrade-module-alwayses — mixed module ──
bool test_upgrade_module_mixed() {
    std::println("\n--- AC6: eda:upgrade-module-alwayses on a mixed module ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    // Build a module with 3 always blocks: 1 comb, 1 ff, 1 multi-edge (stays).
    if (!cs.eval("(define m "
                 "  (make-eda:module 'dff_async "
                 "    (list (make-eda:port 'clk 'input 1) "
                 "          (make-eda:port 'rst 'input 1) "
                 "          (make-eda:port 'd 'input 1) "
                 "          (make-eda:port 'q 'output 1)) "
                 "    (list "
                 "      (make-eda:always '() (list)) "
                 "      (make-eda:always "
                 "        (list (make-eda:sensitivity 'posedge 'clk)) "
                 "        (list)) "
                 "      (make-eda:always "
                 "        (list (make-eda:sensitivity 'posedge 'clk) "
                 "              (make-eda:sensitivity 'negedge 'rst)) "
                 "        (list))))))")) {
        ++g_failed;
        return false;
    }

    if (!cs.eval("(define m-up (eda:upgrade-module-alwayses m))")) {
        ++g_failed;
        return false;
    }

    // Module name preserved.
    auto name = run_symbol_str(cs, "(eda:module-name m-up)");
    CHECK(name == "dff_async", "upgraded module name preserved");

    // First always (was empty sens) → always_comb.
    auto kind1 = run_symbol_str(cs,
        "(eda:always-kind (car (eda:module-body m-up)))");
    CHECK(kind1 == "always_comb",
          "always #1 (empty sens) upgraded to always_comb");

    // Second always (was posedge clk) → always_ff.
    auto kind2 = run_symbol_str(cs,
        "(eda:always-kind (cadr (eda:module-body m-up)))");
    CHECK(kind2 == "always_ff",
          "always #2 (posedge clk) upgraded to always_ff");

    // Third always (multi-edge) → stays 'always.
    auto kind3 = run_symbol_str(cs,
        "(eda:always-kind (caddr (eda:module-body m-up)))");
    CHECK(kind3 == "always",
          "always #3 (multi-edge) stays as 'always");

    // Emitted Verilog shows both comb and ff keywords.
    auto s = run_string(cs, "(eda:emit-verilog m-up)");
    bool has_comb = (s.find("always_comb") != std::string::npos);
    bool has_ff = (s.find("always_ff") != std::string::npos);
    bool has_legacy = (s.find("always @(") != std::string::npos);
    CHECK(has_comb, "upgraded module emit contains 'always_comb'");
    CHECK(has_ff, "upgraded module emit contains 'always_ff'");
    CHECK(has_legacy, "upgraded module still has legacy 'always @(' for the multi-edge one");

    return true;
}

// ── AC7: edge? + single-edge-sensitivity? — the heuristics primitives ──
bool test_heuristic_primitives() {
    std::println("\n--- AC7: eda:edge? and eda:single-edge-sensitivity? ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }
    auto posedge = run_int(cs, "(if (eda:edge? 'posedge) 1 0)");
    CHECK(posedge == 1, "(eda:edge? 'posedge) → #t");

    auto negedge = run_int(cs, "(if (eda:edge? 'negedge) 1 0)");
    CHECK(negedge == 1, "(eda:edge? 'negedge) → #t");

    auto clk = run_int(cs, "(if (eda:edge? 'clk) 1 0)");
    CHECK(clk == 0, "(eda:edge? 'clk) → #f");

    // single-edge-sensitivity? — 1 posedge → #t
    auto single_pe = run_int(cs,
        "(if (eda:single-edge-sensitivity? "
        "      (list (make-eda:sensitivity 'posedge 'clk))) 1 0)");
    CHECK(single_pe == 1, "single posedge sens → #t");

    // 1 level → #f
    auto single_lvl = run_int(cs,
        "(if (eda:single-edge-sensitivity? "
        "      (list (make-eda:sensitivity 'level 'clk))) 1 0)");
    CHECK(single_lvl == 0, "single level sens → #f");

    // empty sens → #f (not a single-edge; treated as combinational)
    auto single_empty = run_int(cs,
        "(if (eda:single-edge-sensitivity? '()) 1 0)");
    CHECK(single_empty == 0, "empty sens → #f (not single-edge)");

    return true;
}

int run_tests() {
    std::println("Issue #436 Phase 2 (automatic always @(*) / @(posedge) → comb / ff)\n");
    test_detect_empty_sens_to_comb();
    test_detect_single_posedge_to_ff();
    test_detect_multi_or_level_keeps_always();
    test_upgrade_always_preserves_body();
    test_upgrade_idempotent();
    test_upgrade_module_mixed();
    test_heuristic_primitives();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_436_phase2_detail

int aura_issue_436_phase2_run() { return aura_issue_436_phase2_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_436_phase2_run(); }
#endif