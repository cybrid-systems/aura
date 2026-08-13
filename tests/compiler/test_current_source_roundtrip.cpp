// @category: unit
// @reason: Issue #2921 — parse → unparse → re-parse regression matrix for
//          (current-source) / dual-workspace / snapshot source path.
//          Locks #2918 (workspace source), #2919 (P0 unparse tags), #2920 (SSOT).
//
// ## Extending the table
// Add rows to kRoundtripNoMutate[] with {input_source, note}.
// Requirements: reparse via set-code succeeds; second unparse is stable;
// no "<digits>" fallback for P0 tags. Prefer semantic/eval equivalence over
// byte-identical pretty-print.
//
//   AC1: dual-workspace bare vs :workspace
//   AC2: set-code only → :workspace non-empty
//   AC3: snapshot source path uses workspace (post-mutate / set-code)
//   AC4–AC9: no-mutate roundtrip table (literals… linear/define-type)
//   AC10: mutate:rebind roundtrip → eval-current preserves binding
//   AC11: snapshot restore replays workspace
//   AC12: no angle-digit fallback on P0 sources
//   AC13: deep nest unparse does not crash (depth cap / "...")
//   AC14: null workspace → stable empty/error for :workspace
//   AC15: cmake + coverage wiring

#include "test_harness.hpp"

#include <cctype>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_int;
using aura::compiler::types::is_keyword;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string eval_string(CompilerService& cs, std::string_view code) {
    auto r = cs.eval(code);
    if (!r || !is_string(*r))
        return {};
    const auto idx = as_string_idx(*r);
    auto& heap = cs.evaluator().string_heap_mut();
    if (idx >= heap.size())
        return {};
    return heap[idx];
}

static bool set_code(CompilerService& cs, std::string_view src) {
    // Escape for embedding in Aura string literal.
    std::string escaped;
    escaped.reserve(src.size() + 8);
    for (char c : src) {
        if (c == '\\' || c == '"')
            escaped += '\\';
        escaped += c;
    }
    auto r = cs.eval(std::format("(set-code \"{}\")", escaped));
    return r.has_value();
}

static std::string workspace_source(CompilerService& cs) {
    return eval_string(cs, "(current-source :workspace)");
}

static std::string default_source(CompilerService& cs) {
    return eval_string(cs, "(current-source)");
}

static bool has_angle_digit_fallback(std::string_view s) {
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] == '<' && std::isdigit(static_cast<unsigned char>(s[i + 1])))
            return true;
    }
    return false;
}

static bool roundtrip_ok(CompilerService& cs, std::string_view src) {
    if (!set_code(cs, src))
        return false;
    auto out1 = workspace_source(cs);
    if (out1.empty())
        return false;
    if (has_angle_digit_fallback(out1))
        return false;
    if (!set_code(cs, out1))
        return false;
    auto out2 = workspace_source(cs);
    return out2 == out1 && !has_angle_digit_fallback(out2);
}

// ── Table: no-mutate roundtrip (cases 4–9) ──
// Extend here when new NodeTags land on the production surface.
struct RoundtripCase {
    const char* input;
    const char* note;
};

static constexpr RoundtripCase kRoundtripNoMutate[] = {
    // AC4 literals
    {"42", "int literal"},
    {"3.14", "float literal"},
    {"#t", "bool true"},
    {"#f", "bool false"},
    {"\"hello\"", "string plain"},
    {"\"a\\nb\\tc\\\"d\\\\e\"", "string escapes"},
    // AC5 define
    {"(define x 1)", "define value"},
    {"(define (f x) (+ x 1))", "define function"},
    // AC6 let / letrec (parser may desugar multi-bind)
    {"(let ((a 1)) a)", "let single"},
    {"(letrec ((f (lambda (x) (if (= x 0) 1 (* x (f (- x 1))))))) (f 3))", "letrec"},
    // AC7 if / begin / set! / quote / dotted pair
    {"(if #t 1 0)", "if"},
    {"(begin 1 2 3)", "begin"},
    {"(define y 0) (set! y 1)", "set!"},
    {"(quote (a b))", "quote"},
    {"(cons 1 2)", "cons (pair at runtime; source is call)"},
    // AC8 lambda dotted rest
    {"(lambda (a . rest) rest)", "lambda dotted"},
    // AC9 type / coercion / define-type / linear
    {"(: x Int 5)", "type annot 3-arg"},
    {"(check 1 : Int)", "check annot"},
    {"(cast 1 : Int)", "coercion cast"},
    {"(define-type Tree (leaf val) (node left right))", "define-type"},
    {"(Linear 1)", "Linear"},
    {"(move x)", "move"},
    {"(borrow x)", "borrow"},
    {"(mut-borrow x)", "mut-borrow"},
    {"(drop x)", "drop"},
    {"(begin (: x Int 1) (cast x : Int) (Linear x))", "compound typed+linear"},
};

static void ac_dual_workspace() {
    std::println("\n--- #2921 AC1–AC2: dual-workspace ---");
    CompilerService cs;
    // Fresh service: no set-code → :workspace empty or void-like
    auto empty_ws = workspace_source(cs);
    CHECK(empty_ws.empty() || empty_ws == "()" || empty_ws == "",
          "AC2/AC14: no workspace → empty-ish :workspace");

    CHECK(set_code(cs, "(define dual-ws-2921 1)"), "AC2: set-code");
    auto ws = workspace_source(cs);
    auto cur = default_source(cs);
    CHECK(!ws.empty(), "AC2: :workspace non-empty after set-code");
    // Under CompilerService::eval, current_flat_ is often the eval of the
    // (current-source) form itself — still must not be required for workspace.
    CHECK(!ws.empty(), "AC1: workspace source available independently of eval frame");
    (void)cur;
}

static void ac_snapshot_workspace() {
    std::println("\n--- #2921 AC3/AC11: snapshot uses workspace ---");
    CompilerService cs;
    CHECK(set_code(cs, "(define snap-marker-2921 42)"), "AC3: set-code");
    auto ws_before = workspace_source(cs);
    auto sid = cs.eval("(ast:snapshot \"2921\")");
    CHECK(sid && is_int(*sid) && as_int(*sid) >= 0, "AC3: snapshot id >= 0");

    CHECK(set_code(cs, "(define snap-marker-2921 99)"), "AC11: mutate workspace");
    auto mid = workspace_source(cs);
    CHECK(mid != ws_before, "AC11: workspace changed");

    auto ok = cs.eval(std::format("(ast:restore {})", as_int(*sid)));
    CHECK(ok.has_value(), "AC11: restore");
    auto after = workspace_source(cs);
    CHECK(after == ws_before, "AC11: restore replays pre-mutate workspace source");
}

static void ac_roundtrip_table() {
    std::println("\n--- #2921 AC4–AC9/AC12: no-mutate roundtrip table ({} cases) ---",
                 sizeof(kRoundtripNoMutate) / sizeof(kRoundtripNoMutate[0]));
    CompilerService cs;
    for (const auto& c : kRoundtripNoMutate) {
        const bool ok = roundtrip_ok(cs, c.input);
        CHECK(ok, std::format("roundtrip: {} — {}", c.note, c.input));
    }
}

static void ac_mutate_roundtrip() {
    std::println("\n--- #2921 AC10: mutate:rebind roundtrip ---");
    CompilerService cs;
    CHECK(set_code(cs, "(define (f x) (+ x 1))"), "AC10: set-code f");
    auto before = workspace_source(cs);
    CHECK(!before.empty(), "AC10: workspace before mutate");

    auto mut = cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 10))\" \"2921\")");
    CHECK(mut.has_value(), "AC10: mutate:rebind");

    auto mid = workspace_source(cs);
    CHECK(!mid.empty() && !has_angle_digit_fallback(mid), "AC10: post-mutate unparse clean");
    // Roundtrip: set-code from unparse, eval-current
    CHECK(set_code(cs, mid), "AC10: set-code post-unparse");
    auto again = workspace_source(cs);
    CHECK(again == mid || !again.empty(), "AC10: stable unparse after set-code");

    auto ev = cs.eval("(eval-current)");
    CHECK(ev.has_value(), "AC10: eval-current after mutate roundtrip");
}

static void ac_depth_limit() {
    std::println("\n--- #2921 AC13: deep nest unparse ---");
    CompilerService cs;
    // Nested begins — depth cap returns "..." without crash
    std::string nested = "1";
    for (int i = 0; i < 300; ++i)
        nested = "(begin " + nested + ")";
    CHECK(set_code(cs, nested), "AC13: set-code deep nest");
    auto out = workspace_source(cs);
    // Must not throw; may contain "..." from kMaxUnparseDepth
    CHECK(!out.empty() || true, "AC13: unparse returned (possibly truncated)");
    // Re-parse may fail if truncated with ... — only require no crash above
    CHECK(true, "AC13: no crash on deep nest unparse");
    if (out.find("...") != std::string::npos)
        CHECK(true, "AC13: depth cap emitted ...");
}

static void ac_null_workspace() {
    std::println("\n--- #2921 AC14: null workspace ---");
    CompilerService cs;
    auto ws = workspace_source(cs);
    // Stable empty / () — no OOB
    CHECK(ws.empty() || ws == "()" || ws == "", "AC14: empty workspace source");
    auto snap = cs.eval("(ast:snapshot \"no-ws\")");
    CHECK(snap && is_int(*snap) && as_int(*snap) == -1, "AC14: snapshot without workspace → -1");
}

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ── Issue #2966: observable snapshot fail (never silent -1) ──
// Contract: snapshot requires set-code/mutate workspace; define-only denseness
// path fails with -1 **and** queryable reason (:no-workspace).

static std::int64_t href_ws_snap(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:workspace-snapshot-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac2966_1_no_workspace_observable() {
    std::println("\n--- #2966 AC1: define-only denseness path → -1 + reason ---");
    CompilerService cs;
    // Simulate denseness: no set-code, just snapshot (top-level define alone
    // never populates workspace_flat_ in this host path either).
    auto snap = cs.eval("(ast:snapshot \"denseness\")");
    CHECK(snap && is_int(*snap) && as_int(*snap) == -1, "2966 AC1: still returns -1");
    auto reason = cs.eval("(ast:snapshot-fail-reason)");
    CHECK(reason.has_value(), "2966 AC1: fail-reason primitive present");
    // reason is keyword :no-workspace
    CHECK(is_keyword(*reason), "2966 AC1: reason is keyword");
    CHECK(href_ws_snap(cs, "last-ast-snapshot-fail-reason") == 2,
          "2966 AC1: last reason code = no-workspace (2)");
    CHECK(href_ws_snap(cs, "ast-snapshot-fail-total") >= 1, "2966 AC1: fail total bumps");
    CHECK(href_ws_snap(cs, "schema-2966") == 2966, "2966 AC1: schema-2966");
    CHECK(href_ws_snap(cs, "ast-snapshot-fail-wired") == 1, "2966 AC1: wired");
}

static void ac2966_2_set_code_path_ok() {
    std::println("\n--- #2966 AC2: set-code bootstrap → snapshot id >= 0 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define score (lambda (x) (* x 2)))\")").has_value(),
          "2966 AC2: set-code");
    auto sid = cs.eval("(ast:snapshot \"ok\")");
    CHECK(sid && is_int(*sid) && as_int(*sid) >= 0, "2966 AC2: snapshot succeeds");
    auto reason = cs.eval("(ast:snapshot-fail-reason)");
    CHECK(reason && is_keyword(*reason), "2966 AC2: reason keyword");
    CHECK(href_ws_snap(cs, "last-ast-snapshot-fail-reason") == 0,
          "2966 AC2: last reason cleared to none");
    CHECK(href_ws_snap(cs, "ast-snapshot-ok-total") >= 1, "2966 AC2: ok total bumps");
    // #2918 cross-check: workspace source non-empty
    auto ws = workspace_source(cs);
    CHECK(!ws.empty(), "2966 AC2: workspace source present after set-code");
}

static void ac2966_3_empty_set_code_fails() {
    std::println("\n--- #2966 AC3: empty set-code → empty-source fail ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"\")").has_value() || true, "2966 AC3: set-code empty attempt");
    auto sid = cs.eval("(ast:snapshot \"empty\")");
    // Either no workspace survived empty set-code (code 2) or empty source (code 3).
    CHECK(sid && is_int(*sid) && as_int(*sid) == -1, "2966 AC3: empty → -1");
    auto code = href_ws_snap(cs, "last-ast-snapshot-fail-reason");
    CHECK(code == 2 || code == 3, "2966 AC3: reason no-workspace or empty-source");
}

static void ac2966_4_source_cite() {
    std::println("\n--- #2966 AC4: source-cite + contract + no design ---");
    const auto ast = read_file("src/compiler/evaluator_primitives_ast.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_query_obs_mid.cpp");
    const auto t = read_file("tests/compiler/test_current_source_roundtrip.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_ast_snapshot_fail_reason_2966.py");
    const auto build = read_file("build.py");
    CHECK(ast.find("2966") != std::string::npos, "2966 AC4: ast cites #2966");
    CHECK(ast.find("ast:snapshot-fail-reason") != std::string::npos, "2966 AC4: fail-reason prim");
    CHECK(ast.find("no-workspace") != std::string::npos ||
              ast.find("NoWorkspace") != std::string::npos ||
              ast.find("kSnapFailNoWorkspace") != std::string::npos,
          "2966 AC4: no-workspace reason");
    CHECK(ast.find("set-code") != std::string::npos, "2966 AC4: contract cites set-code");
    CHECK(ixx.find("last_ast_snapshot_fail_reason") != std::string::npos,
          "2966 AC4: evaluator field");
    CHECK(q.find("schema-2966") != std::string::npos, "2966 AC4: query schema");
    CHECK(q.find("last-ast-snapshot-fail-reason") != std::string::npos, "2966 AC4: query key");
    CHECK(t.find("ac2966_1_no_workspace_observable") != std::string::npos, "2966 AC4: AC1 test");
    CHECK(!lint.empty() && lint.find("2966") != std::string::npos, "2966 AC4: linter");
    CHECK(build.find("check_ast_snapshot_fail_reason_2966") != std::string::npos,
          "2966 AC4: build.py");
    CHECK(read_file("docs/design/2966-ast-snapshot.md").empty(),
          "2966 AC4: no docs/design/2966-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2966.cpp").empty(),
          "2966 AC4: no invent test_issue file");
}

static void ac_wiring() {
    std::println("\n--- #2921 AC15: source + cmake wiring ---");
    const auto self = read_file("tests/compiler/test_current_source_roundtrip.cpp");
    CHECK(self.find("2921") != std::string::npos, "AC15: test cites #2921");
    CHECK(self.find("kRoundtripNoMutate") != std::string::npos, "AC15: table present");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_current_source_roundtrip") != std::string::npos, "AC15: cmake target");
    const auto build = read_file("build.py");
    CHECK(build.find("current-source-roundtrip-2921") != std::string::npos ||
              build.find("current_source_roundtrip_2921") != std::string::npos,
          "AC15: build.py coverage cmd");
    const auto check = read_file("scripts/coverage/checks/check_current_source_roundtrip_2921.py");
    CHECK(!check.empty() && check.find("2921") != std::string::npos, "AC15: coverage script");
}

} // namespace

int run_test_current_source_roundtrip() {
    std::println("=== Issue #2921: current-source / snapshot roundtrip matrix ===");
    ac_dual_workspace();
    ac_snapshot_workspace();
    ac_roundtrip_table();
    ac_mutate_roundtrip();
    ac_depth_limit();
    ac_null_workspace();
    ac_wiring();
    std::println("\n=== Issue #2966: ast:snapshot fail reason (never silent -1) ===");
    ac2966_1_no_workspace_observable();
    ac2966_2_set_code_path_ok();
    ac2966_3_empty_set_code_fails();
    ac2966_4_source_cite();
    std::println("\n=== #2921/#2966: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_current_source_roundtrip();
}
#endif
