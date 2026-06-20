// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_161.cpp — Issue #161 Phase 2: parser is now a pure function.
//
// Verifies (a) parse_to_flat is the only public entry point (no
// FlatParser class), (b) calling it twice on the same input gives
// the same output (determinism), (c) parse_to_flat does not
// require any state from the caller beyond (source, flat, pool),
// (d) FlatParseResult has success/root/error fields as before.
//
// Phase 2 ships: class FlatParser is gone, replaced by
// detail::parse_X(ParserState& s, ...) free functions.
// parse_to_flat is the public API — pure function with all state
// stack-allocated inside the call.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;
import aura.core.ast;
import aura.parser.parser;



namespace aura_issue_161_detail {
#define PRINTLN(msg) do { std::print("{}\n", std::string(msg)); } while(0)

// ── Test 1: parse_to_flat exists and is callable (no FlatParser needed) ──
bool test_parse_to_flat_basic() {
    PRINTLN("\n--- Test 1: parse_to_flat is the public entry point ---");
    aura::ast::FlatAST flat1;
    aura::ast::StringPool pool1;
    auto r1 = aura::parser::parse_to_flat("42", flat1, pool1);
    CHECK(r1.success, "parse_to_flat(\"42\") returned success");
    CHECK(r1.root != aura::ast::NULL_NODE, "parse_to_flat(\"42\") returned a root node");
    CHECK(r1.error.empty(), "parse_to_flat(\"42\") had no error");
    return true;
}

// ── Test 2: determinism — calling twice gives the same result ──
bool test_determinism() {
    PRINTLN("\n--- Test 2: parse_to_flat is deterministic ---");

    const char* sources[] = {
        "42",
        "(+ 1 2)",
        "(define x 10) (define y 20) (+ x y)",
        "(lambda (a b) (* a b))",
        "(if (> x 0) x (- x))",
        "(let ((a 1) (b 2)) (+ a b))",
        "(quote (1 2 3))",
        "(list 1 2 3 4 5 6 7 8 9 10)",
    };
    constexpr int N = sizeof(sources) / sizeof(sources[0]);

    for (int i = 0; i < N; ++i) {
        aura::ast::FlatAST flat1, flat2;
        aura::ast::StringPool pool1, pool2;
        auto r1 = aura::parser::parse_to_flat(sources[i], flat1, pool1);
        auto r2 = aura::parser::parse_to_flat(sources[i], flat2, pool2);
        std::string label = std::string("both parses succeeded: ") + sources[i];
        CHECK(r1.success && r2.success, label);
        std::string label2 = std::string("same root node id for: ") + sources[i];
        CHECK(r1.root == r2.root, label2);
        if (r1.root != aura::ast::NULL_NODE) {
            auto v1 = flat1.get(r1.root);
            auto v2 = flat2.get(r2.root);
            std::string l3 = std::string("same root tag for: ") + sources[i];
            std::string l4 = std::string("same child count for: ") + sources[i];
            CHECK(v1.tag == v2.tag, l3);
            CHECK(v1.children.size() == v2.children.size(), l4);
        }
    }
    return true;
}

// ── Test 3: parse_to_flat doesn't share state between calls ──
//
// The parser is a pure function: each call constructs a fresh
// ParserState internally. After the first call, the second call
// should not see any "leftover" tokens from the first. We test
// by interleaving parses of different sources into the same
// FlatAST. If the parser leaked state, later parses would fail
// or produce wrong structures.
bool test_state_isolation() {
    PRINTLN("\n--- Test 3: state isolation between calls ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;

    // Interleave: a, b, a, b, a — each call should produce the
    // correct structure regardless of what came before.
    auto ra1 = aura::parser::parse_to_flat("(+ 1 2)", flat, pool);
    auto rb1 = aura::parser::parse_to_flat("(* 3 4)", flat, pool);
    auto ra2 = aura::parser::parse_to_flat("(+ 1 2)", flat, pool);
    auto rb2 = aura::parser::parse_to_flat("(* 3 4)", flat, pool);
    auto ra3 = aura::parser::parse_to_flat("(+ 1 2)", flat, pool);

    CHECK(ra1.success && ra2.success && ra3.success, "all \"a\" parses succeeded");
    CHECK(rb1.success && rb2.success, "all \"b\" parses succeeded");
    // All \"a\" parses should produce the same AST structure.
    auto va1 = flat.get(ra1.root);
    auto va2 = flat.get(ra2.root);
    auto va3 = flat.get(ra3.root);
    CHECK(va1.tag == va2.tag && va2.tag == va3.tag,
          "all \"a\" parses produce same root tag (no state leak)");
    CHECK(va1.children.size() == va2.children.size() && va2.children.size() == va3.children.size(),
          "all \"a\" parses produce same child count (no state leak)");
    // All \"b\" parses should produce the same AST structure.
    auto vb1 = flat.get(rb1.root);
    auto vb2 = flat.get(rb2.root);
    CHECK(vb1.tag == vb2.tag, "all \"b\" parses produce same root tag (no state leak)");
    CHECK(vb1.children.size() == vb2.children.size(),
          "all \"b\" parses produce same child count (no state leak)");
    return true;
}

// ── Test 4: edge cases ──
bool test_edge_cases() {
    PRINTLN("\n--- Test 4: edge cases (sanity) ---");
    {
        aura::ast::FlatAST flat;
        aura::ast::StringPool pool;
        auto r = aura::parser::parse_to_flat("", flat, pool);
        CHECK(!r.success, "empty input fails (no expression)");
    }
    {
        aura::ast::FlatAST flat;
        aura::ast::StringPool pool;
        auto r = aura::parser::parse_to_flat("   \n  ", flat, pool);
        CHECK(!r.success, "whitespace-only input fails");
    }
    {
        aura::ast::FlatAST flat;
        aura::ast::StringPool pool;
        auto r = aura::parser::parse_to_flat("(quote (1 2 3))", flat, pool);
        CHECK(r.success, "quoted list parsed");
    }
    {
        aura::ast::FlatAST flat;
        aura::ast::StringPool pool;
        std::string deep = "(";
        for (int i = 0; i < 50; ++i) deep += "(";
        deep += "1";
        for (int i = 0; i < 50; ++i) deep += ")";
        deep += ")";
        auto r = aura::parser::parse_to_flat(deep, flat, pool);
        CHECK(r.success, "deeply nested (51 levels) parsed without recursion overflow");
    }
    {
        aura::ast::FlatAST flat;
        aura::ast::StringPool pool;
        std::string too_deep = "(";
        for (int i = 0; i < 510; ++i) too_deep += "(";
        too_deep += "1";
        for (int i = 0; i < 510; ++i) too_deep += ")";
        too_deep += ")";
        auto r = aura::parser::parse_to_flat(too_deep, flat, pool);
        CHECK(!r.success, "over-recursion (>500 levels) rejected by depth guard");
    }
    return true;
}

// ── Test 5: independent FlatASTs ──
//
// Two completely separate FlatASTs parsing the same source
// should both end up with equivalent AST structures. This
// proves the parser doesn't share any state between the two
// contexts (a property the old FlatParser could not guarantee
// because it held mutable state across calls).
bool test_independent_flatast() {
    PRINTLN("\n--- Test 5: two independent FlatASTs both get correct content ---");
    aura::ast::FlatAST flat1, flat2;
    aura::ast::StringPool pool1, pool2;
    auto r1 = aura::parser::parse_to_flat("(+ 1 2)", flat1, pool1);
    auto r2 = aura::parser::parse_to_flat("(+ 1 2)", flat2, pool2);
    CHECK(r1.success && r2.success, "both parses succeeded");
    if (r1.root != aura::ast::NULL_NODE && r2.root != aura::ast::NULL_NODE) {
        auto v1 = flat1.get(r1.root);
        auto v2 = flat2.get(r2.root);
        CHECK(v1.tag == v2.tag, "both roots have same tag");
        CHECK(v1.children.size() == v2.children.size(), "both roots have same number of children");
        // Walk both trees and verify structural equivalence
        for (std::size_t i = 0; i < v1.children.size(); ++i) {
            auto c1 = flat1.get(v1.children[i]);
            auto c2 = flat2.get(v2.children[i]);
            CHECK(c1.tag == c2.tag, "child tags match at index");
        }
    }
    return true;
}

int run_tests() {
    std::fprintf(stdout, "═══ Issue #161 — Phase 2: pure-function parser ═══\n");

    test_parse_to_flat_basic();
    test_determinism();
    test_state_isolation();
    test_edge_cases();
    test_independent_flatast();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_161_detail

int aura_issue_161_run() { return aura_issue_161_detail::run_tests(); }

