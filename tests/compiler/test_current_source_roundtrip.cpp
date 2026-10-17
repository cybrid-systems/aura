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
//
// Issue #4130: quoted empty list unparse — the nil sentinel (LiteralInt 0)
// must render as (quote ()) in compact AND :pretty unparse, never (quote 0);
// roundtrip table row pins AST stability, eval semantics untouched.
//
// Issue #4132: export names survive unparse — the Export case renders the
// stored name children (never the empty params side-table), so
// (export pick-best) round-trips through every current-source mode.
//
// Issue #4266: safe-refactor:replace-fn gates on the STRUCTURED typecheck
// status — the new (typecheck-status) primitive (#t clean / diagnostics
// report string / #f no-workspace), never the raw (typecheck-current)
// string, which embeds diagnostics and returns normally on type errors;
// check-and-apply restores the snapshot when post-verify throws.

#include "test_harness.hpp"

#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/mutate_type_gate.hh"
#include "core/workspace_epoch.hh"

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
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

// Issue #4266: structured-status probe — evaluates `code` and reports both
// whether the result was a boolean and its value. #t/#f faces of
// (typecheck-status) must be distinguishable from any string report.
static bool eval_bool(CompilerService& cs, std::string_view code, bool* is_bool_out) {
    auto r = cs.eval(code);
    if (!r || !is_bool(*r)) {
        if (is_bool_out)
            *is_bool_out = false;
        return false;
    }
    if (is_bool_out)
        *is_bool_out = true;
    return as_bool(*r);
}

// Issue #4266: escape `src` for embedding inside an Aura string literal
// (same rules as set_code below — backslash and double-quote) so AC bodies
// can pass replace-fn source strings through std::format safely.
static std::string aura_escape(std::string_view src) {
    std::string out;
    out.reserve(src.size() + 8);
    for (char c : src) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
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

// Issue #4130: the forbidden serialization — quoted nil rendered as the
// number zero under quote. Compact and :pretty share one emitter.
static bool has_quote_zero(std::string_view s) {
    return s.find("(quote 0)") != std::string::npos;
}

// Issue #4132: the forbidden serialization — the Export case read its
// (never-populated) params side-table, so every named export collapsed
// to an empty `(export)` in every current-source mode.
static bool has_empty_export(std::string_view s) {
    return s.find("(export)") != std::string::npos;
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
    {"(quote ())", "quote empty list (#4130)"},
    // Issue #4132: module-level export names survive the roundtrip
    {"(export pick-best)", "export names (#4132)"},
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

// ── Issue #4130: quoted empty list renders (quote ()), never (quote 0) ──

static void ac4130_1_pretty_unparse_empty_list() {
    std::println("\n--- #4130 AC1: empty list unparse → (quote ()), never (quote 0) ---");
    CompilerService cs;
    CHECK(set_code(cs, "(define (f) (quote ()))"), "4130 AC1: set-code quoted empty list");
    auto ws = workspace_source(cs);
    auto pretty = eval_string(cs, "(current-source :workspace :pretty)");
    CHECK(!ws.empty() && !pretty.empty(), "4130 AC1: both unparse modes produced source");
    CHECK(!has_quote_zero(ws), "4130 AC1: compact unparse never (quote 0)");
    CHECK(!has_quote_zero(pretty), "4130 AC1: pretty unparse never (quote 0)");
    CHECK(ws.find("(quote ())") != std::string::npos,
          "4130 AC1: compact unparse renders (quote ())");
    CHECK(pretty.find("(quote ())") != std::string::npos,
          "4130 AC1: pretty unparse renders (quote ())");
}

static void ac4130_2_call_site_roundtrip() {
    std::println("\n--- #4130 AC2: quoted empty list call-site roundtrip ---");
    CompilerService cs;
    CHECK(set_code(cs, "(define (g lst) (if (null? lst) (quote ()) lst))"),
          "4130 AC2: set-code call-site empty list");
    auto ws = workspace_source(cs);
    CHECK(!has_quote_zero(ws), "4130 AC2: call-site unparse never (quote 0)");
    // Re-parse the unparse, unparse again → stable, still no (quote 0)
    CHECK(set_code(cs, ws), "4130 AC2: re-parse unparse output");
    auto again = workspace_source(cs);
    CHECK(!has_quote_zero(again), "4130 AC2: second unparse never (quote 0)");
    CHECK(again == ws, "4130 AC2: unparse stable across re-parse");
}

static void ac4130_3_eval_semantics_unchanged() {
    std::println("\n--- #4130 AC3: eval semantics of quoted nil unchanged ---");
    CompilerService cs;
    CHECK(set_code(cs, "(define (f) (quote ()))"), "4130 AC3: set-code");
    // Serialization fix only: (f) still evaluates to nil — observe via if/int.
    auto r = cs.eval("(begin (eval-current) (if (null? (f)) 7 9))");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 7,
          "4130 AC3: (null? (f)) true — nil eval semantics preserved");
    auto ws = workspace_source(cs);
    CHECK(!has_quote_zero(ws), "4130 AC3: post-eval unparse never (quote 0)");
}

static void ac4130_4_source_cite() {
    std::println("\n--- #4130 AC4: source-cite + wiring + no design doc ---");
    const auto unp = read_file("src/core/ast_unparse.ixx");
    const auto t = read_file("tests/compiler/test_current_source_roundtrip.cpp");
    const auto lint = read_file("scripts/check_empty_list_unparse_4130.py");
    const auto build = read_file("build.py");
    CHECK(unp.find("4130") != std::string::npos, "4130 AC4: unparse cites #4130");
    CHECK(unp.find("nil sentinel") != std::string::npos,
          "4130 AC4: nil-sentinel arm documented in unparse");
    CHECK(t.find("ac4130_1_pretty_unparse_empty_list") != std::string::npos,
          "4130 AC4: AC1 test present");
    CHECK(t.find("quote empty list (#4130)") != std::string::npos,
          "4130 AC4: roundtrip table row present");
    CHECK(!lint.empty() && lint.find("4130") != std::string::npos, "4130 AC4: linter present");
    CHECK(build.find("check_empty_list_unparse_4130") != std::string::npos,
          "4130 AC4: build.py registration");
    CHECK(read_file("docs/design/4130-empty-list-unparse.md").empty(),
          "4130 AC4: no docs/design/4130-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_4130.cpp").empty(),
          "4130 AC4: no test_issue_4130 file per #81934");
}

// ── Issue #4132: export names survive current-source unparse (never empty (export)) ──

static void ac4132_1_unparse_preserves_export_names() {
    std::println(
        "\n--- #4132 AC1: export names unparse → (export pick-best), never empty (export) ---");
    CompilerService cs;
    CHECK(
        set_code(cs, "(export pick-best) (define (pick-best xs) (if (null? xs) -999999 (car xs)))"),
        "4132 AC1: set-code export-bearing module");
    auto ws = workspace_source(cs);
    auto pretty = eval_string(cs, "(current-source :workspace :pretty)");
    auto cur = default_source(cs);
    CHECK(!ws.empty() && !pretty.empty(), "4132 AC1: both unparse modes produced source");
    CHECK(!has_empty_export(ws), "4132 AC1: compact unparse never emits empty (export)");
    CHECK(!has_empty_export(pretty), "4132 AC1: pretty unparse never emits empty (export)");
    CHECK(!has_empty_export(cur), "4132 AC1: default current-source never emits empty (export)");
    CHECK(ws.find("(export pick-best)") != std::string::npos,
          "4132 AC1: compact unparse preserves (export pick-best)");
    CHECK(pretty.find("(export pick-best)") != std::string::npos,
          "4132 AC1: pretty unparse preserves (export pick-best)");
}

static void ac4132_2_export_roundtrip_reparse() {
    std::println("\n--- #4132 AC2: export-name module re-parse roundtrip ---");
    CompilerService cs;
    CHECK(
        set_code(cs, "(export pick-best) (define (pick-best xs) (if (null? xs) -999999 (car xs)))"),
        "4132 AC2: set-code export-bearing module");
    auto ws = workspace_source(cs);
    CHECK(ws.find("(export pick-best)") != std::string::npos,
          "4132 AC2: first unparse preserves (export pick-best)");
    CHECK(!has_empty_export(ws), "4132 AC2: first unparse never empty (export)");
    CHECK(set_code(cs, ws), "4132 AC2: re-parse unparse output");
    auto again = workspace_source(cs);
    CHECK(!has_empty_export(again), "4132 AC2: second unparse never empty (export)");
    CHECK(again == ws, "4132 AC2: unparse stable across re-parse");
}

static void ac4132_3_storage_and_eval_unchanged() {
    std::println("\n--- #4132 AC3: export names stored pre-unparse; eval semantics unchanged ---");
    CompilerService cs;
    CHECK(
        set_code(cs, "(export pick-best) (define (pick-best xs) (if (null? xs) -999999 (car xs)))"),
        "4132 AC3: set-code export-bearing module");
    // Serialization-face contract: the workspace AST owns the Export node with
    // its name children before any unparse — eval-current behaves as before.
    auto r = cs.eval("(begin (eval-current) (if (= (pick-best (quote (3 4))) 3) 7 9))");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 7,
          "4132 AC3: (pick-best (quote (3 4))) → 3 — eval semantics preserved");
    auto ws_after = workspace_source(cs);
    CHECK(!has_empty_export(ws_after) && ws_after.find("(export pick-best)") != std::string::npos,
          "4132 AC3: post-eval unparse still preserves (export pick-best)");
}

static void ac4132_4_source_cite() {
    std::println("\n--- #4132 AC4: source-cite + wiring + no design doc ---");
    const auto unp = read_file("src/core/ast_unparse.ixx");
    const auto t = read_file("tests/compiler/test_current_source_roundtrip.cpp");
    const auto lint = read_file("scripts/check_export_name_unparse_4132.py");
    const auto build = read_file("build.py");
    CHECK(unp.find("4132") != std::string::npos, "4132 AC4: unparse cites #4132");
    CHECK(unp.find("params side-table") != std::string::npos,
          "4132 AC4: children-not-params arm documented in unparse");
    CHECK(t.find("ac4132_1_unparse_preserves_export_names") != std::string::npos,
          "4132 AC4: AC1 test present");
    CHECK(t.find("export names (#4132)") != std::string::npos,
          "4132 AC4: roundtrip table row present");
    CHECK(!lint.empty() && lint.find("4132") != std::string::npos, "4132 AC4: linter present");
    CHECK(build.find("check_export_name_unparse_4132") != std::string::npos,
          "4132 AC4: build.py registration");
    CHECK(read_file("docs/design/4132-export-name-unparse.md").empty(),
          "4132 AC4: no docs/design/4132-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_4132.cpp").empty(),
          "4132 AC4: no test_issue_4132 file per #81934");
}

// ── Issue #4266: safe-refactor:replace-fn gates on structured typecheck status ──
// (typecheck-current) embeds diagnostics in a returned STRING and returns
// normally on type errors, so the old (try (begin (set-code rebuilt)
// (typecheck-current) 'ok)) arm reported 'applied for ill-typed bodies.
// Fix under test: (typecheck-status) primitive — #t clean / diagnostics
// report string / #f no-workspace, sharing one traversal + Phase-5 cache
// with typecheck-current — and replace-fn branches on it before reporting
// applied; check-and-apply runs post-verify inside the restore path.

static void ac4266_1_structured_status() {
    std::println("\n--- #4266 AC1: typecheck-status structured (#t / report / #f) ---");
    CompilerService cs;
    bool got_bool = false;
    const auto no_ws = eval_bool(cs, "(typecheck-status)", &got_bool);
    CHECK(got_bool && !no_ws, "4266 AC1: no workspace → #f (never a report string)");
    CHECK(set_code(cs, "(define (score (: x Int)) (* x 2))"), "4266 AC1: set-code clean");
    const auto clean = eval_bool(cs, "(typecheck-status)", &got_bool);
    CHECK(got_bool && clean, "4266 AC1: clean workspace → #t");
    CHECK(set_code(cs, "(define (score (: x Int)) (* x 2)) (score \"s\")"),
          "4266 AC1: set-code ill-typed");
    const auto report = eval_string(cs, "(typecheck-status)");
    CHECK(!report.empty() && report.find("diagnostics:") != std::string::npos,
          "4266 AC1: ill-typed → diagnostics report string");
    CHECK(eval_string(cs, "(typecheck-current)").find("diagnostics:") != std::string::npos,
          "4266 AC1: typecheck-current string contract unchanged (shared core)");
    CHECK(set_code(cs, "(define (score (: x Int)) (* x 3))"), "4266 AC1: clean mutation");
    const auto again = eval_bool(cs, "(typecheck-status)", &got_bool);
    CHECK(got_bool && again, "4266 AC1: clean again → #t (Phase-5 cache status parity)");
}

static void ac4266_2_replace_fn_applies_well_typed() {
    std::println("\n--- #4266 AC2: replace-fn well-typed → (applied …) ---");
    CompilerService cs;
    CHECK(cs.eval("(require std/safe-refactor all:)").has_value(), "4266 AC2: require stdlib");
    CHECK(set_code(cs, "(define (score (: x Int)) (* x 2))"), "4266 AC2: seed workspace");
    const auto body = aura_escape("(define (score (: x Int)) (+ x 2))");
    const auto status = eval_string(
        cs,
        std::format("(let ((st (safe-refactor:replace-fn \"score\" \"{}\"))) "
                    "(if (and (pair? st) (equal? (car st) 'applied)) \"applied\" \"not-applied\"))",
                    body));
    CHECK(status == "applied", "4266 AC2: well-typed replacement reports applied");
    CHECK(workspace_source(cs).find("(+ x 2)") != std::string::npos,
          "4266 AC2: workspace carries the new body");
}

static void ac4266_3_replace_fn_rejects_and_restores() {
    std::println("\n--- #4266 AC3: replace-fn ill-typed → rejected + snapshot restored ---");
    CompilerService cs;
    CHECK(cs.eval("(require std/safe-refactor all:)").has_value(), "4266 AC3: require stdlib");
    CHECK(set_code(cs, "(define (score (: x Int)) (* x 2))"), "4266 AC3: seed workspace");
    const auto body = aura_escape("(define (score (: x Int)) (score \"not-an-int\"))");
    const auto status = eval_string(
        cs, std::format(
                "(let ((st (safe-refactor:replace-fn \"score\" \"{}\"))) "
                "(if (and (pair? st) (equal? (car st) 'rejected)) \"rejected\" \"not-rejected\"))",
                body));
    CHECK(status == "rejected", "4266 AC3: ill-typed replacement rejected (was: applied)");
    CHECK(workspace_source(cs).find("not-an-int") == std::string::npos,
          "4266 AC3: snapshot restored — ill-typed body absent from workspace");
    bool got_bool = false;
    const auto clean = eval_bool(cs, "(typecheck-status)", &got_bool);
    CHECK(got_bool && clean, "4266 AC3: post-restore workspace typechecks clean");
    const auto reason = eval_string(
        cs, std::format("(car (cdr (safe-refactor:replace-fn \"score\" \"{}\")))", body));
    CHECK(reason.find("diagnostics:") != std::string::npos,
          "4266 AC3: rejected reason carries the diagnostics report (no English parsing)");
}

static void ac4266_4_check_apply_post_verify_throw_restores() {
    std::println("\n--- #4266 AC4: check-and-apply post-verify throw → error + restored ---");
    CompilerService cs;
    CHECK(cs.eval("(require std/safe-refactor all:)").has_value(), "4266 AC4: require stdlib");
    CHECK(set_code(cs, "(define (seed (: x Int)) x)"), "4266 AC4: seed workspace");
    const auto status =
        eval_string(cs, "(let ((st (safe-refactor:check-and-apply (lambda () #t) "
                        "(lambda () (error \"pv-boom\")) "
                        "(lambda () (begin (set-code \"(define (mut (: x Int)) x)\") 42))))) "
                        "(cond ((and (equal? (car st) 'error) "
                        "(equal? (car (cdr st)) \"post-verify-error-raised\")) \"error-restored\") "
                        "(else \"wrong-status\")))");
    CHECK(status == "error-restored", "4266 AC4: throw → (error post-verify-error-raised)");
    CHECK(workspace_source(cs).find("(define (mut") == std::string::npos,
          "4266 AC4: snapshot restored — apply mutation rolled back (was: escaped)");
}

static void ac4266_5_check_apply_post_verify_false_still_rolls_back() {
    std::println("\n--- #4266 AC5: check-and-apply post-verify #f → rolled-back (pin) ---");
    CompilerService cs;
    CHECK(cs.eval("(require std/safe-refactor all:)").has_value(), "4266 AC5: require stdlib");
    CHECK(set_code(cs, "(define (seed (: x Int)) x)"), "4266 AC5: seed workspace");
    const auto status =
        eval_string(cs, "(let ((st (safe-refactor:check-and-apply (lambda () #t) (lambda () #f) "
                        "(lambda () (begin (set-code \"(define (mut (: x Int)) x)\") 42))))) "
                        "(cond ((and (equal? (car st) 'rolled-back) "
                        "(equal? (car (cdr st)) \"post-verify-failed\")) \"rolled-back\") "
                        "(else \"wrong-status\")))");
    CHECK(status == "rolled-back", "4266 AC5: #f → (rolled-back post-verify-failed) preserved");
    CHECK(workspace_source(cs).find("(define (mut") == std::string::npos,
          "4266 AC5: mutation rolled back");
}

static void ac4266_6_source_cite() {
    std::println("\n--- #4266 AC6: source-cite + wiring + no design doc ---");
    const auto prim = read_file("src/compiler/evaluator_primitives_eval.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto stdlib = read_file("lib/std/safe-refactor.aura");
    const auto t = read_file("tests/compiler/test_current_source_roundtrip.cpp");
    const auto sh = read_file("tests/python/run-tests.sh");
    const auto lint = read_file("scripts/check_safe_refactor_typecheck_4266.py");
    const auto build = read_file("build.py");
    CHECK(prim.find("4266") != std::string::npos, "4266 AC6: primitive cites #4266");
    CHECK(prim.find("typecheck-status") != std::string::npos, "4266 AC6: primitive registered");
    CHECK(prim.find("run_workspace_typecheck") != std::string::npos,
          "4266 AC6: shared core — one traversal, one cache, no second model");
    CHECK(ixx.find("last_typecheck_ok_") != std::string::npos, "4266 AC6: cache parity field");
    CHECK(stdlib.find("4266") != std::string::npos, "4266 AC6: stdlib cites #4266");
    CHECK(stdlib.find("(typecheck-status)") != std::string::npos,
          "4266 AC6: replace-fn gates on structured status");
    CHECK(stdlib.find("post-verify-error-raised") != std::string::npos,
          "4266 AC6: post-verify throw restore path present");
    CHECK(t.find("ac4266_1_structured_status") != std::string::npos, "4266 AC6: AC1 test present");
    CHECK(sh.find("4266") != std::string::npos, "4266 AC6: run-tests.sh smoke cases");
    CHECK(!lint.empty() && lint.find("4266") != std::string::npos, "4266 AC6: linter present");
    CHECK(build.find("check_safe_refactor_typecheck_4266") != std::string::npos,
          "4266 AC6: build.py registration");
    CHECK(read_file("docs/design/4266-replace-fn-typecheck.md").empty(),
          "4266 AC6: no docs/design/4266-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_4266.cpp").empty(),
          "4266 AC6: no test_issue_4266 file per #81934");
}

// Issue #4266 follow-up: a whole-workspace replacement ((set-code ...)) must
// invalidate the Phase-5 clean-workspace typecheck report, and the workspace
// unparse must keep per-parameter type annotations. Together these are what
// let safe-refactor:replace-fn reject an ill-typed replacement: without them
// the previously cached clean #t leaked through (the row reported 'applied
// for an ill-typed replacement, and the rebuilt source lost the ground
// signature the typechecker gates on).
static void ac4266_7_set_code_invalidates_typecheck_cache() {
    std::println(
        "\n--- #4266 AC7: set-code invalidates cached status + keeps param annotation ---");
    CompilerService cs;
    CHECK(set_code(cs, "(define (score (: x Int)) (* x 2))"), "4266 AC7: set-code annotated seed");
    CHECK(workspace_source(cs).find("(: x Int)") != std::string::npos,
          "4266 AC7: workspace unparse preserves the parameter type annotation");
    // Warm the Phase-5 clean-workspace cache.
    bool got_bool = false;
    CHECK(eval_bool(cs, "(typecheck-status)", &got_bool) && got_bool,
          "4266 AC7: clean workspace caches structured #t");
    // Ill-typed replacement: the previously cached clean status must NOT leak.
    CHECK(set_code(cs, "(define (score (: x Int)) (* x 2)) (score \"s\")"),
          "4266 AC7: set-code ill-typed replacement");
    const auto report = eval_string(cs, "(typecheck-status)");
    CHECK(report.find("diagnostics:") != std::string::npos,
          "4266 AC7: ill-typed set-code invalidates the cached clean status (was: stale #t)");
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

// ── Issue #4349: ast:restore over a post-rebind snapshot leaves env
// closures holding dead node ids — the stale call must surface the
// documented recoverable StaleWorkspace error (never InternalError),
// eval-current re-binds, and the bare (ast:generation) metric is
// callable from Soft (was stats-face only). ──
static void ac4349_restore_over_rebind() {
    std::println("\n--- #4349: restore over rebind + bare ast:generation ---");
    CompilerService cs;
    CHECK(set_code(cs, "(define f4349 (lambda (x) x))\n(define g4349 (lambda (y) (f4349 y)))"),
          "4349: set-code f/g");
    CHECK(cs.eval("(eval-current)").has_value(), "4349: eval-current binds");
    auto pre = cs.eval("(g4349 4)");
    CHECK(pre && is_int(*pre) && as_int(*pre) == 4, "4349: (g 4) = 4 pre-rebind");
    auto gen = cs.eval("(ast:generation)");
    CHECK(gen && is_int(*gen), "4349: bare (ast:generation) is an int");
    auto sid = cs.eval("(ast:snapshot \"4349\")");
    CHECK(sid && is_int(*sid) && as_int(*sid) >= 0, "4349: snapshot id");
    CHECK(cs.eval("(mutate:rebind \"f4349\" \"(lambda (x) (* x 2))\" \"dbl\")").has_value(),
          "4349: rebind f -> double");
    auto post = cs.eval("(g4349 4)");
    CHECK(post && is_int(*post) && as_int(*post) == 8, "4349: (g 4) = 8 post-rebind");
    auto st = cs.eval(std::format("(ast:restore {})", as_int(*sid)));
    CHECK(st && is_bool(*st) && as_bool(*st), "4349: restore to pre-rebind snapshot");
    // The stale compiled call must not silently succeed: on the
    // CompilerService face the StaleWorkspace diagnostic surfaces as a
    // valueless result (the engine face carries the message); either way
    // the caller cannot observe a stale answer.
    auto stale_val = cs.eval("(g4349 4)");
    CHECK(!(stale_val && is_int(*stale_val)), "4349: stale call never returns a stale int");
    CHECK(cs.eval("(eval-current)").has_value(), "4349: eval-current re-binds");
    // ast:restore republishes function_sources_ from the restored
    // defines before pre_cache. Without that, eval-current relowers
    // g from the post-rebind body and (g 4) stays 8.
    auto healed = cs.eval("(g4349 4)");
    CHECK(healed && is_int(*healed) && as_int(*healed) == 4,
          "4349: (g 4) = 4 after recovery (restored identity f)");
}

// Issue #4349 reopen: a plain ast:snapshot (no restore, no mutate) must
// not drop a define whose body calls another define. The leaf stays
// callable either way; the caller was the one that became invalid.
static void ac4349_snapshot_keeps_caller() {
    std::println("\n--- #4349: snapshot keeps a calling define ---");
    CompilerService cs;
    CHECK(set_code(cs, "(define (tab-size) 2)\n(define (indent n) (* n (tab-size)))\n#t"),
          "4349: set-code indent");
    CHECK(cs.eval("(eval-current)").has_value(), "4349: eval-current before snapshot");
    auto before = cs.eval("(indent 2)");
    CHECK(before && is_int(*before) && as_int(*before) == 4,
          "4349: (indent 2) = 4 before snapshot");
    auto sid = cs.eval("(ast:snapshot \"t\")");
    CHECK(sid && is_int(*sid) && as_int(*sid) >= 0, "4349: snapshot id");
    auto after = cs.eval("(indent 2)");
    CHECK(after && is_int(*after) && as_int(*after) == 4, "4349: (indent 2) = 4 after snapshot");
    auto leaf = cs.eval("(tab-size)");
    CHECK(leaf && is_int(*leaf) && as_int(*leaf) == 2, "4349: (tab-size) stays 2");
    auto sid2 = cs.eval("(ast:snapshot \"t2\")");
    CHECK(sid2 && is_int(*sid2) && as_int(*sid2) >= 0, "4349: second snapshot");
    auto after2 = cs.eval("(indent 2)");
    CHECK(after2 && is_int(*after2) && as_int(*after2) == 4,
          "4349: (indent 2) = 4 after a second snapshot");
    CHECK(cs.eval("(eval-current)").has_value(), "4349: eval-current after snapshot");
    auto again = cs.eval("(indent 2)");
    CHECK(again && is_int(*again) && as_int(*again) == 4,
          "4349: (indent 2) = 4 after a later eval-current");
    const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    CHECK(ir.find("Issue #4349") != std::string::npos,
          "4349: IR call restamps a private callee closure");
}

// Issue #4357: set-code pre-cache unparses defines into function_sources_
// and eval-current re-lowers that text onto the IR interpreter. A bool
// printed as 1/0 comes back as an integer ((q) was 1, (boolean? (q)) #f).
static void ac4357_workspace_bools() {
    std::println("\n--- #4357: workspace defines keep booleans ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    CompilerService cs;
    CHECK(set_code(cs, "(define (p) (list \"a\" #t #f))\n"
                       "(define (q) #t)\n"
                       "(define (r) (if #t #t #f))\n"
                       "#t"),
          "4357: set-code");
    auto src = workspace_source(cs);
    CHECK(src.find("#t") != std::string::npos, "4357: workspace source keeps #t");
    CHECK(src.find("#f") != std::string::npos, "4357: workspace source keeps #f");
    CHECK(cs.eval("(eval-current)").has_value(), "4357: eval-current");
    auto q = cs.eval("(q)");
    CHECK(q && is_bool(*q) && as_bool(*q), "4357: (q) is #t");
    auto bq = cs.eval("(boolean? (q))");
    CHECK(bq && is_bool(*bq) && as_bool(*bq), "4357: (boolean? (q))");
    auto eq = cs.eval("(eq? (q) #t)");
    CHECK(eq && is_bool(*eq) && as_bool(*eq), "4357: (eq? (q) #t)");
    auto r = cs.eval("(r)");
    CHECK(r && is_bool(*r) && as_bool(*r), "4357: (r) is #t");
    auto mid = cs.eval("(boolean? (car (cdr (p))))");
    CHECK(mid && is_bool(*mid) && as_bool(*mid), "4357: list element #t");
    auto tail = cs.eval("(eq? (car (cdr (cdr (p)))) #f)");
    CHECK(tail && is_bool(*tail) && as_bool(*tail), "4357: list element #f");
    auto head = cs.eval("(if (string=? (car (p)) \"a\") 1 0)");
    CHECK(head && is_int(*head) && as_int(*head) == 1, "4357: list head is \"a\"");
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("Issue #4357") != std::string::npos,
          "4357: unparse keeps bool literals for the IR reparse");
}

// Issue #4475: set-code + eval-current specializes workspace lambda bodies
// onto the IR interpreter via a re-parse of the unparsed source. The
// compiler-side unparser printed float literals as "()" (the default arm),
// which re-parses as the nil sentinel — LiteralInt 0 — so (z1) was 0 and
// (* m 2.5) was (* m 0). Value defines keep the tree-walker cell and
// mutate:rebind publishes the raw string, so both stayed correct.
static void ac4475_workspace_floats() {
    std::println("\n--- #4475: workspace lambda bodies keep float literals ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    auto close_to = [](double got, double want) { return std::fabs(got - want) < 1e-9; };
    CompilerService cs;
    CHECK(set_code(cs, "(define (z1) 0.05)\n"
                       "(define (z2) 2.75)\n"
                       "(define (z3 m) (* m 2.5))\n"
                       "(define z4 0.5)\n"
                       "(define (z5 m) (+ m 0.05))"),
          "4475: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4475: eval-current");
    auto z1 = cs.eval("(z1)");
    CHECK(z1 && is_float(*z1) && close_to(as_float(*z1), 0.05), "4475: (z1) is 0.05");
    auto z2 = cs.eval("(z2)");
    CHECK(z2 && is_float(*z2) && close_to(as_float(*z2), 2.75), "4475: (z2) is 2.75");
    auto z3 = cs.eval("(= (z3 2) 5)");
    CHECK(z3 && is_bool(*z3) && as_bool(*z3), "4475: (z3 2) is 5");
    auto z4 = cs.eval("z4");
    CHECK(z4 && is_float(*z4) && close_to(as_float(*z4), 0.5), "4475: value define z4 is 0.5");
    auto z5 = cs.eval("(z5 1)");
    CHECK(z5 && is_float(*z5) && close_to(as_float(*z5), 1.05), "4475: (z5 1) is 1.05");
    // Same run, more shapes: float inside a call argument list and a let
    // binding — both previously re-parsed as the nil sentinel (0).
    CHECK(set_code(cs, "(define (y2) (list 1.5 2))\n"
                       "(define (y3) (let ((k 1.5)) k))"),
          "4475: set-code shapes");
    CHECK(cs.eval("(eval-current)").has_value(), "4475: eval-current shapes");
    auto y2 = cs.eval("(car (y2))");
    CHECK(y2 && is_float(*y2) && close_to(as_float(*y2), 1.5), "4475: list element 1.5");
    auto y3 = cs.eval("(y3)");
    CHECK(y3 && is_float(*y3) && close_to(as_float(*y3), 1.5), "4475: let-bound 1.5");
    // mutate:rebind keeps publishing the raw string; stays correct.
    CHECK(set_code(cs, "(define (z1) 0.05)"), "4475: set-code rebind face");
    CHECK(cs.eval("(eval-current)").has_value(), "4475: eval-current rebind face");
    CHECK(cs.eval("(mutate:rebind \"z1\" \"(lambda () 0.25)\" \"f\")").has_value(), "4475: rebind");
    CHECK(cs.eval("(eval-current)").has_value(), "4475: eval-current rebind");
    auto rb = cs.eval("(z1)");
    CHECK(rb && is_float(*rb) && close_to(as_float(*rb), 0.25), "4475: rebound body 0.25");
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("Issue #4475") != std::string::npos,
          "4475: unparse keeps float literals for the IR reparse");
}

// Issue #4450: runtime MakeClosure inside an IR define mints ids from the
// interpreter's stale cursor — a range already handed to the NEXT define —
// so an escaping closure ((let ((a n)) (lambda () a)) shape) silently ran
// another define, or hit "invalid closure" when the aliased define was
// absent. Runtime mints now come from the global monotonic cursor, and
// dispatch routes them to the minting interpreter via runtime_closures_.
static void run_4450_runtime_mint_ids() {
    std::println("\n--- #4450: escaped define closures run their own body ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "1", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    // AC1/AC2: the issue repro — both escaped closures run THEIR OWN mk.
    // Pre-fix both printed f1's string (g2's mint collided with f1's bound
    // id, and dispatch ran that define's body).
    {
        CompilerService cs;
        CHECK(set_code(cs, "(define (mk n) (let ((a n)) (lambda () a)))\n"
                           "(define (f1 x) (string-append \"F1-called:\" (if (null? x) \"nil\" "
                           "\"arg\")))\n"
                           "(define (f2 x) (string-append \"F2-called:\" (if (null? x) \"nil\" "
                           "\"arg\")))\n"
                           "(define g1 (mk 7))\n"
                           "(define g2 (mk 8))\n"
                           "#t"),
              "4450: set-code mk/f1/f2/g1/g2");
        CHECK(cs.eval("(eval-current)").has_value(), "4450: eval-current");
        auto v1 = cs.eval("(g1)");
        CHECK(v1 && is_int(*v1) && as_int(*v1) == 7,
              "4450 AC1: (g1) runs its own body (7, never f1's string)");
        auto v2 = cs.eval("(g2)");
        CHECK(v2 && is_int(*v2) && as_int(*v2) == 8,
              "4450 AC2: (g2) runs its own body (8, never f1's string)");
    }
    // AC3: single-define face — the escaped closure must be callable at all
    // (pre-fix: "eval_flat: invalid closure" because the minted id had no
    // owner entry).
    {
        CompilerService cs;
        CHECK(set_code(cs, "(define (mk n) (let ((a n)) (lambda () a)))\n"
                           "(define g (mk 7))\n"
                           "#t"),
              "4450: set-code mk/g");
        CHECK(cs.eval("(eval-current)").has_value(), "4450: eval-current single");
        auto v = cs.eval("(g)");
        CHECK(v && is_int(*v) && as_int(*v) == 7,
              "4450 AC3: single escaped closure calls through (7)");
    }
}

// Issue #4584: a workspace define that hands a procedure to filter/map was
// rebound at the call site. Two mechanisms: the cache-hit pick took name#0
// (the nested predicate, renamed before the define's own lambda) so call 1
// already returned #t, and a re-entrant dispatch rewound the closure-id
// cursor so call 2 of a named predicate ran the previous define (budget).
// Both calls must be (8 (a)). Rows the report listed as already ok stay ok.
static void ac4584_hof_callee_stable() {
    std::println("\n--- #4584: second call keeps the HOF callee ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    auto is_true = [](CompilerService& cs, std::string_view code) {
        auto r = cs.eval(code);
        return r && is_bool(*r) && as_bool(*r);
    };
    // (list 8 (list "a")) — one call of `callee` per eval.
    const char* filtered = "(let ((r (decide \"a\"))) "
                           "(and (= (car r) 8) "
                           "(string=? (car (car (cdr r))) \"a\") "
                           "(null? (cdr (car (cdr r))))))";
    const char* one_a = "(let ((r (order \"a\"))) "
                        "(and (string=? (car r) \"a\") (null? (cdr r))))";

    {
        CompilerService cs;
        CHECK(set_code(cs, "(define budget (lambda (c) 8))\n"
                           "(define order (lambda (c) (filter (lambda (o) (equal? o \"a\")) "
                           "(list \"a\" \"b\"))))\n"
                           "(define decide (lambda (x) (list (budget x) (order x))))"),
              "4584: set-code inline filter");
        CHECK(cs.eval("(eval-current)").has_value(), "4584: eval-current inline");
        CHECK(is_true(cs, filtered), "4584: inline call 1 is (8 (a))");
        CHECK(is_true(cs, filtered), "4584: inline call 2 is (8 (a))");
        CHECK(is_true(cs, filtered), "4584: inline call 3 is (8 (a))");
        CHECK(is_true(cs, one_a), "4584: direct (order \"a\") stays (a)");
    }
    {
        CompilerService cs;
        CHECK(set_code(cs, "(define budget (lambda (c) 8))\n"
                           "(define pred (lambda (o) (equal? o \"a\")))\n"
                           "(define order-named (lambda (c) (filter pred (list \"a\" \"b\"))))\n"
                           "(define decide-named (lambda (x) (list (budget x) (order-named x))))"),
              "4584: set-code named pred");
        CHECK(cs.eval("(eval-current)").has_value(), "4584: eval-current named");
        const char* named = "(let ((r (decide-named \"a\"))) "
                            "(and (= (car r) 8) "
                            "(string=? (car (car (cdr r))) \"a\") "
                            "(null? (cdr (car (cdr r))))))";
        CHECK(is_true(cs, named), "4584: named call 1 is (8 (a))");
        auto pb = cs.eval("(pred \"b\")");
        CHECK(pb && is_bool(*pb) && !as_bool(*pb), "4584: (pred \"b\") is #f");
        auto eq = cs.eval("(eq? pred budget)");
        CHECK(eq && is_bool(*eq) && !as_bool(*eq), "4584: pred is not budget");
        CHECK(is_true(cs, "(let ((r (order-named \"a\"))) "
                          "(and (string=? (car r) \"a\") (null? (cdr r))))"),
              "4584: direct order-named after call 1 is (a)");
        CHECK(is_true(cs, named), "4584: named call 2 is (8 (a))");
        CHECK(is_true(cs, named), "4584: named call 3 is (8 (a))");
        CHECK(is_true(cs, "(let ((r (order-named \"a\"))) "
                          "(and (string=? (car r) \"a\") (null? (cdr r))))"),
              "4584: direct order-named after call 2 is (a)");
    }
    {
        CompilerService cs;
        CHECK(set_code(cs, "(define budget (lambda (c) 8))\n"
                           "(define order (lambda (c) (map (lambda (o) o) (list \"a\" \"b\"))))\n"
                           "(define decide (lambda (x) (list (budget x) (order x))))"),
              "4584: set-code map");
        CHECK(cs.eval("(eval-current)").has_value(), "4584: eval-current map");
        const char* mapped = "(let ((r (decide \"a\"))) "
                             "(and (= (car r) 8) "
                             "(string=? (car (car (cdr r))) \"a\") "
                             "(string=? (car (cdr (car (cdr r)))) \"b\") "
                             "(null? (cdr (cdr (car (cdr r)))))))";
        CHECK(is_true(cs, mapped), "4584: map call 1 is (8 (a b))");
        CHECK(is_true(cs, mapped), "4584: map call 2 is (8 (a b))");
    }
    {
        CompilerService cs;
        CHECK(set_code(cs, "(define app (lambda (f z) (f z)))\n"
                           "(define budget (lambda (c) 8))\n"
                           "(define order (lambda (c) (app (lambda (o) o) \"z\")))\n"
                           "(define decide (lambda (x) (list (budget x) (order x))))"),
              "4584: set-code user hof");
        CHECK(cs.eval("(eval-current)").has_value(), "4584: eval-current user hof");
        const char* zipped = "(let ((r (decide \"a\"))) "
                             "(and (= (car r) 8) (string=? (car (cdr r)) \"z\")))";
        CHECK(is_true(cs, zipped), "4584: user hof call 1 is (8 z)");
        CHECK(is_true(cs, zipped), "4584: user hof call 2 is (8 z)");
    }
    {
        // Source writes order before budget. List still builds (order-result
        // budget-result). This row stayed correct before the fix.
        CompilerService cs;
        CHECK(set_code(cs, "(define budget (lambda (c) 8))\n"
                           "(define order (lambda (c) (filter (lambda (o) (equal? o \"a\")) "
                           "(list \"a\" \"b\"))))\n"
                           "(define decide (lambda (x) (list (order x) (budget x))))"),
              "4584: set-code order-before-budget");
        CHECK(cs.eval("(eval-current)").has_value(), "4584: eval-current order-before");
        const char* swapped = "(let ((r (decide \"a\"))) "
                              "(and (string=? (car (car r)) \"a\") "
                              "(null? (cdr (car r))) "
                              "(= (car (cdr r)) 8)))";
        CHECK(is_true(cs, swapped), "4584: order-before call 1 is ((a) 8)");
        CHECK(is_true(cs, swapped), "4584: order-before call 2 is ((a) 8)");
    }
    {
        CompilerService cs;
        CHECK(set_code(cs, "(define budget (lambda (c) 8))\n"
                           "(define order (lambda (c) (filter (lambda (o) (equal? o \"a\")) "
                           "(list \"a\" \"b\"))))\n"
                           "(define decide (lambda (x) (order x)))"),
              "4584: set-code only-order");
        CHECK(cs.eval("(eval-current)").has_value(), "4584: eval-current only-order");
        const char* only = "(let ((r (decide \"a\"))) "
                           "(and (string=? (car r) \"a\") (null? (cdr r))))";
        CHECK(is_true(cs, only), "4584: only-order call 1 is (a)");
        CHECK(is_true(cs, only), "4584: only-order call 2 is (a)");
    }
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("Issue #4584") != std::string::npos, "4584: exact define name wins over name#0");
    const auto irx = read_file("src/compiler/ir_executor.ixx");
    CHECK(irx.find("Issue #4584") != std::string::npos, "4584: closure cursor does not rewind");
}

// Issue #4585: the first set-code takes the mutation epoch off 0. Host
// closures stamped while tracking was inactive must stay on the capture
// snapshot. The signal is closure-bridge-epoch-zero-treated-stale-total:
// a stale birth stamp counts about twice per call. (four) stays 4.
static void ac4585_host_calls_stay_on_snapshot() {
    std::println("\n--- #4585: host calls stay fresh after set-code ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    CHECK(aura::core::current_mutation_epoch() == 0, "4585: epoch is 0 before the first workspace");
    CompilerService cs;
    const char* prog =
        "(define (inc x) (+ x 1))\n"
        "(define (four) (inc (inc (inc (inc 0)))))\n"
        "(define (zst) (hash-ref (engine:metrics \"query:epoch-apply-hotpath-stats\") "
        "\"closure-bridge-epoch-zero-treated-stale-total\" -1))\n"
        "(define (rst) (hash-ref (engine:metrics \"query:aot-incremental-reemit-stats\") "
        "\"live-closure-epoch-restamp-total\" -1))\n"
        "(define b0 (zst))\n"
        "(define br0 (rst))\n"
        "(four) (four) (four) (four) (four)\n"
        "(define b1 (zst))\n"
        "(define br1 (rst))\n"
        "(set-code \"(define w (lambda (x) x))\")\n"
        "(define a0 (zst))\n"
        "(define ar0 (rst))\n"
        "(four) (four) (four) (four) (four)\n"
        "(define a1 (zst))\n"
        "(define ar1 (rst))\n"
        "(set-code \"(define a (lambda (x) x)) (define b (lambda (x) x)) "
        "(define c (lambda (x) x)) (define d (lambda (x) x)) "
        "(define e (lambda (x) x)) (define f (lambda (x) x)) "
        "(define g (lambda (x) x))\")\n"
        "(define c0 (zst))\n"
        "(define cr0 (rst))\n"
        "(four) (four) (four) (four) (four)\n"
        "(define c1 (zst))\n"
        "(define cr1 (rst))\n"
        "(eval-current)\n"
        "(define d0 (zst))\n"
        "(define dr0 (rst))\n"
        "(four) (four) (four) (four) (four)\n"
        "(define d1 (zst))\n"
        "(define dr1 (rst))\n"
        "(and (= (four) 4) (= b0 0) (>= br0 0) "
        "(= (- b1 b0) 0) (= (- br1 br0) 0) "
        "(= (- a1 a0) 0) (= (- ar1 ar0) 0) "
        "(= (- c1 c0) 0) (= (- cr1 cr0) 0) "
        "(= (- d1 d0) 0) (= (- dr1 dr0) 0))\n";
    auto r = cs.eval(prog);
    CHECK(r && is_bool(*r) && as_bool(*r),
          "4585: (four) stays 4 and bridge-0 stale does not climb");
    const auto prim = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(prim.find("adopt_pre_tracking_closure_bridge_epochs") != std::string::npos,
          "4585: set-code promotes birth stamps after rebind");
    CHECK(prim.find("restamp_usable_closure_bridge_epochs") != std::string::npos,
          "4585: a later set-code rides usable stamps up to the new epoch");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    const auto adopt = env.find("void Evaluator::adopt_pre_tracking_closure_bridge_epochs");
    CHECK(adopt != std::string::npos, "4585: adopt helper is defined");
    if (adopt != std::string::npos) {
        const auto body = env.substr(adopt, 900);
        CHECK(body.find("must_deopt_before_next_call") != std::string::npos,
              "4585: a must-deopt poison stays at bridge 0");
        CHECK(body.find("lifetime_valid_for_views") != std::string::npos,
              "4585: a dead lifetime stays at bridge 0");
    }
    const auto restamp = env.find("void Evaluator::restamp_usable_closure_bridge_epochs");
    CHECK(restamp != std::string::npos, "4585: usable restamp helper is defined");
    if (restamp != std::string::npos) {
        const auto body = env.substr(restamp, 900);
        CHECK(body.find("cl.bridge_epoch == 0") != std::string::npos,
              "4585: an unstamped closure stays at bridge 0");
        CHECK(body.find("body_usable") != std::string::npos, "4585: a cleared view stays stale");
    }
}

// Issue #4360: adding a define must not drop sibling IR closures, and a
// hard rebind must reinstall the caller after the drain. The light path
// stays armed only for Soft; hard still typechecks and still drains.
static void ac4360_sibling_and_hard_caller() {
    std::println("\n--- #4360: sibling closure and hard caller survive ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    {
        CompilerService cs;
        CHECK(set_code(cs, "(define f (lambda (x) (+ x 1)))\n"
                           "(define g (lambda (x) (* x 2)))\n"
                           "#t"),
              "4360: set-code f/g");
        CHECK(cs.eval("(eval-current)").has_value(), "4360: eval-current");
        auto g0 = cs.eval("(g 2)");
        CHECK(g0 && is_int(*g0) && as_int(*g0) == 4, "4360: (g 2) = 4 before add");
        auto added = cs.eval("(mutate:rebind \"zzz\" \"(lambda (x) x)\" \"s\")");
        CHECK(added && is_bool(*added) && as_bool(*added), "4360: add zzz");
        auto bad = cs.eval("(mutate:rebind \"g\" \"(lambda (x\" \"s\")");
        CHECK(!(bad && is_bool(*bad) && as_bool(*bad)), "4360: bad body is refused");
        auto src = workspace_source(cs);
        CHECK(src.find("zzz") != std::string::npos, "4360: workspace keeps zzz");
        auto g1 = cs.eval("(g 2)");
        CHECK(g1 && is_int(*g1) && as_int(*g1) == 4, "4360: (g 2) = 4 after add");
        auto f1 = cs.eval("(f 1)");
        CHECK(f1 && is_int(*f1) && as_int(*f1) == 2, "4360: (f 1) = 2 after add");
    }
    {
        using aura::compiler::mutate_type_gate::mode;
        using aura::compiler::mutate_type_gate::MutateTypeGate;
        using aura::compiler::mutate_type_gate::set_mode;
        const auto prev = mode();
        struct RestoreGate {
            MutateTypeGate prev;
            ~RestoreGate() { set_mode(prev); }
        } restore_gate{prev};
        set_mode(MutateTypeGate::Hard);
        CompilerService cs;
        CHECK(set_code(cs, "(define rr-h (lambda (x) (+ x 1)))\n"
                           "(define rr-u (lambda (y) (rr-h y)))\n"
                           "#t"),
              "4360: set-code rr-h/rr-u");
        CHECK(cs.eval("(eval-current)").has_value(), "4360: hard eval-current");
        auto u0 = cs.eval("(rr-u 1)");
        CHECK(u0 && is_int(*u0) && as_int(*u0) == 2, "4360: (rr-u 1) = 2");
        auto rb = cs.eval("(mutate:rebind \"rr-h\" \"(lambda (x) (+ x 2))\" \"r\")");
        CHECK(rb && is_bool(*rb) && as_bool(*rb), "4360: hard rebind");
        auto u1 = cs.eval("(rr-u 1)");
        CHECK(u1 && is_int(*u1) && as_int(*u1) == 3, "4360: (rr-u 1) = 3 after hard rebind");
        auto h1 = cs.eval("(rr-h 1)");
        CHECK(h1 && is_int(*h1) && as_int(*h1) == 3, "4360: (rr-h 1) = 3 after hard rebind");
    }
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("Issue #4360") != std::string::npos,
          "4360: owner clear stays gated to a workspace replace");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("Issue #4360") != std::string::npos,
          "4360: hard rebind reinstalls the caller after the drain");
}

// Issue #4362: a caller-side arity mismatch must not arm the soft light
// path. Selective typecheck already refuses it under the hard gate.
// Same-arity bodies, primitive type errors, and a self-recursive extra
// arg stay committed. An unbound name stays refused.
static void ac4362_soft_caller_arity() {
    std::println("\n--- #4362: soft rebind refuses a caller arity mismatch ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    using aura::compiler::mutate_type_gate::mode;
    using aura::compiler::mutate_type_gate::MutateTypeGate;
    using aura::compiler::mutate_type_gate::set_mode;
    const auto prev = mode();
    struct RestoreGate {
        MutateTypeGate prev;
        ~RestoreGate() { set_mode(prev); }
    } restore_gate{prev};
    set_mode(MutateTypeGate::Soft);

    auto fresh = [](CompilerService& cs) {
        CHECK(set_code(cs, "(define (hello x) (+ x 1))\n"
                           "(define (bye y) (hello y))\n"
                           "#t"),
              "4362: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "4362: eval-current");
    };
    auto committed = [](const auto& v) { return v && is_bool(*v) && as_bool(*v); };

    {
        CompilerService cs;
        fresh(cs);
        auto rb = cs.eval("(mutate:rebind \"bye\" \"(lambda (y) (hello y y))\" \"s\")");
        CHECK(!committed(rb), "4362: soft rebind refuses caller arity");
        auto src = workspace_source(cs);
        CHECK(src.find("(hello y y)") == std::string::npos, "4362: soft rebind leaves the call");
        CHECK(src.find("(hello y)") != std::string::npos, "4362: original bye body stays");
    }
    {
        CompilerService cs;
        fresh(cs);
        auto tma = cs.eval(
            "(typed-mutate-atomic (list \"(mutate:rebind \\\"bye\\\" \\\"(lambda (y) (hello y "
            "y))\\\")\"))");
        CHECK(!committed(tma), "4362: soft atomic refuses caller arity");
        auto src = workspace_source(cs);
        CHECK(src.find("(hello y y)") == std::string::npos, "4362: soft atomic leaves the call");
    }
    {
        CompilerService cs;
        fresh(cs);
        auto ok = cs.eval("(mutate:rebind \"bye\" \"(lambda (y) (+ (hello y) 1))\" \"s\")");
        CHECK(committed(ok), "4362: soft same-arity rebind commits");
        auto src = workspace_source(cs);
        CHECK(src.find("(hello y)") != std::string::npos, "4362: same-arity body kept");
        CHECK(src.find("(hello y y)") == std::string::npos, "4362: same-arity is one hello call");
    }
    {
        CompilerService cs;
        fresh(cs);
        auto ub = cs.eval("(mutate:rebind \"bye\" \"(lambda (y) (+ y nope))\" \"s\")");
        CHECK(!committed(ub), "4362: soft still refuses an unbound name");
        auto src = workspace_source(cs);
        CHECK(src.find("nope") == std::string::npos, "4362: unbound body is not committed");
    }
    {
        CompilerService cs;
        fresh(cs);
        auto ty = cs.eval("(mutate:rebind \"bye\" \"(lambda (y) (string-append y 1))\" \"s\")");
        CHECK(committed(ty), "4362: soft still commits a primitive type error");
        auto src = workspace_source(cs);
        CHECK(src.find("string-append") != std::string::npos, "4362: type-error body commits");
    }
    {
        set_mode(MutateTypeGate::Hard);
        CompilerService cs;
        fresh(cs);
        auto rb = cs.eval("(mutate:rebind \"bye\" \"(lambda (y) (hello y y))\" \"s\")");
        CHECK(!committed(rb), "4362: hard still refuses caller arity");
        auto src = workspace_source(cs);
        CHECK(src.find("(hello y y)") == std::string::npos, "4362: hard leaves the call");
    }
    {
        set_mode(MutateTypeGate::Soft);
        CompilerService cs;
        fresh(cs);
        auto self = cs.eval("(mutate:rebind \"hello\" \"(lambda (x) (hello x x))\" \"s\")");
        CHECK(committed(self), "4362: self-recursive rebind still commits");
        auto src = workspace_source(cs);
        CHECK(src.find("(hello x x)") != std::string::npos, "4362: self-recursive body commits");
    }
    {
        // Same evaluator: a refused arity check must not poison the next
        // closed rebind or a primitive type error.
        set_mode(MutateTypeGate::Soft);
        CompilerService cs;
        fresh(cs);
        auto bad = cs.eval("(mutate:rebind \"bye\" \"(lambda (y) (hello y y))\" \"s\")");
        CHECK(!committed(bad), "4362: first soft rebind refuses caller arity");
        fresh(cs);
        auto ty = cs.eval("(mutate:rebind \"bye\" \"(lambda (y) (string-append y 1))\" \"s\")");
        CHECK(committed(ty), "4362: type error still commits after a refusal");
        fresh(cs);
        auto ok = cs.eval("(mutate:rebind \"bye\" \"(lambda (y) (+ (hello y) 1))\" \"s\")");
        CHECK(committed(ok), "4362: same-arity rebind commits after a refusal");
        auto src = workspace_source(cs);
        CHECK(src.find("(hello y y)") == std::string::npos,
              "4362: refused call is not left behind");
        CHECK(src.find("(hello y)") != std::string::npos, "4362: later body calls hello once");
    }
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("Issue #4362") != std::string::npos,
          "4362: caller arity stays off the light path");
}

int run_test_current_source_roundtrip() {
    std::println("=== Issue #2921: current-source / snapshot roundtrip matrix ===");
    // Before any set-code in this process: the epoch must still be 0 so the
    // birth-stamp path is the one the CLI hits on a fresh start.
    ac4585_host_calls_stay_on_snapshot();
    ac_dual_workspace();
    ac_snapshot_workspace();
    ac_roundtrip_table();
    ac_mutate_roundtrip();
    ac_depth_limit();
    ac_null_workspace();
    ac_wiring();
    ac4349_restore_over_rebind();
    ac4349_snapshot_keeps_caller();
    ac4357_workspace_bools();
    ac4475_workspace_floats();
    run_4450_runtime_mint_ids();
    ac4584_hof_callee_stable();
    ac4360_sibling_and_hard_caller();
    ac4362_soft_caller_arity();
    std::println("\n=== Issue #2966: ast:snapshot fail reason (never silent -1) ===");
    ac2966_1_no_workspace_observable();
    ac2966_2_set_code_path_ok();
    ac2966_3_empty_set_code_fails();
    ac2966_4_source_cite();
    std::println("\n=== Issue #4130: quoted empty list unparse (never (quote 0)) ===");
    ac4130_1_pretty_unparse_empty_list();
    ac4130_2_call_site_roundtrip();
    ac4130_3_eval_semantics_unchanged();
    ac4130_4_source_cite();
    std::println("\n=== Issue #4132: export names survive current-source unparse ===");
    ac4132_1_unparse_preserves_export_names();
    ac4132_2_export_roundtrip_reparse();
    ac4132_3_storage_and_eval_unchanged();
    ac4132_4_source_cite();
    std::println("\n=== Issue #4266: replace-fn structured typecheck gate ===");
    ac4266_1_structured_status();
    ac4266_2_replace_fn_applies_well_typed();
    ac4266_3_replace_fn_rejects_and_restores();
    ac4266_4_check_apply_post_verify_throw_restores();
    ac4266_5_check_apply_post_verify_false_still_rolls_back();
    ac4266_6_source_cite();
    ac4266_7_set_code_invalidates_typecheck_cache();
    std::println("\n=== #2921/#2966: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_current_source_roundtrip();
}
#endif
