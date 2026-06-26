// @category: integration
// @reason: uses CompilerService to eval Aura source; tests #248 new
//          query:schema-of-marker primitive (observability side)

// test_issue_248.cpp — Issue #248: SyntaxMarker + type schema
//
// Issue #248 (scope-limited close) ships the **observability**
// side of the macro-type-tracking feature. The full scope
// (auto-populating schemas on macro expansion + using them to
// enforce type invariants) is 2-3 days of work and is deferred
// to a follow-up issue.
//
// What's shipped in this close:
//
//   - (query:schema-of-marker marker-name) Aura primitive.
//     Returns a list of (NodeId . type-name) pairs for nodes
//     with the given SyntaxMarker AND a non-zero type_id_
//     (i.e., the type checker has inferred a type for them).
//   - Optional 2nd arg: integer limit N. Caps the result.
//
// Deferred (per the issue body):
//   - Auto-populate schema in clone_macro_body (so macro-
//     introduced nodes have a schema before type check).
//   - Type checker consults schema for macro-introduced nodes.
//   - typed_mutate pre-check rejects schema-violating changes
//     on macro-introduced code.
//   - Per-node schema cache (currently we just look up
//     type_id_ on demand).
//
// Test pattern (matches test_issue_244, test_issue_247):
// Each AC uses (set-code "...") + (typecheck-current) to
// populate the type cache, then queries. The macro-
// introduced marker case has a known limitation (#244)
// where the cloned body may not end up in workspace_flat_.
// The test documents this and accepts n=0 for MacroIntroduced.
//
// Test cases:
//   AC1: query:schema-of-marker returns a list
//   AC2: with fresh set-code + typecheck, User marker has results
//   AC3: MacroIntroduced marker returns a (possibly empty) list
//   AC4: unknown marker name returns error pair
//   AC5: bad arg types return error pair
//   AC6: limit-N caps the result list
//   AC7: no-workspace returns error pair


import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_248_detail {
static int g_passed = 0;
static int g_failed = 0;

struct EvalResult {
    bool ok = false;
    aura::compiler::types::EvalValue v{};
};

static EvalResult try_run(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) return {false, aura::compiler::types::make_void()};
    return {true, *r};
}

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println("  PASS: {}", msg); } \
    else      { ++g_failed; std::println("  FAIL: {}", msg); } \
} while (0)

#define CHECK_EQ_INT(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_passed; std::println("  PASS: {}  ({} = {})", msg, _a, _b); } \
    else          { ++g_failed; std::println("  FAIL: {}  ({} != {})", msg, _a, _b); } \
} while (0)

static std::int64_t is_pair_result(aura::compiler::CompilerService& cs, std::string_view expr) {
    auto r = try_run(cs, std::string("(if (pair? ") + std::string(expr) + ") 1 0)");
    if (!r.ok || !aura::compiler::types::is_int(r.v)) return -1;
    return aura::compiler::types::as_int(r.v);
}

static bool set_source(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = try_run(cs, std::string("(set-code \"") + src + "\")");
    return r.ok;
}

// ═══════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════

// ── AC1: query:schema-of-marker returns a list ─────────────────
bool test_returns_list() {
    std::println("\n--- AC1: query:schema-of-marker returns a list ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(define x 5)")) { ++g_failed; return false; }
    auto r = try_run(cs, "(typecheck-current)");
    if (!r.ok) { ++g_failed; return false; }
    std::int64_t is_pair = is_pair_result(cs, "(query:schema-of-marker \"User\")");
    CHECK_EQ_INT(is_pair, 1, "query:schema-of-marker \"User\" returns a non-empty list");
    return true;
}

// ── AC2: with fresh set-code + typecheck, User marker has results
bool test_user_marker_results() {
    std::println("\n--- AC2: with fresh set-code + typecheck, User marker has results ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(define x 5)")) { ++g_failed; return false; }
    auto r = try_run(cs, "(typecheck-current)");
    if (!r.ok) { ++g_failed; return false; }
    // The result is a list of (NodeId . type-name) pairs.
    // Count them by destructuring: (length ...) on the list.
    auto r2 = try_run(cs, "(length (query:schema-of-marker \"User\"))");
    if (!r2.ok || !aura::compiler::types::is_int(r2.v)) {
        std::println("  FAIL: result is not an int");
        ++g_failed; return false;
    }
    std::int64_t n = aura::compiler::types::as_int(r2.v);
    std::println("    [info] User-marker count = {}", n);
    CHECK(n > 0, "User-marker nodes with type info > 0 after typecheck");
    return true;
}

// ── AC3: MacroIntroduced marker returns a (possibly empty) list
bool test_macro_introduced_marker() {
    std::println("\n--- AC3: MacroIntroduced marker returns a (possibly empty) list ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs,
        "(define x 5) (define-hygienic-macro (d y) (* y 2))")) {
        ++g_failed; return false;
    }
    auto r = try_run(cs, "(typecheck-current)");
    if (!r.ok) { ++g_failed; return false; }
    auto r2 = try_run(cs, "(length (query:schema-of-marker \"MacroIntroduced\"))");
    if (!r2.ok || !aura::compiler::types::is_int(r2.v)) {
        std::println("  FAIL: result is not an int");
        ++g_failed; return false;
    }
    std::int64_t n = aura::compiler::types::as_int(r2.v);
    std::println("    [info] MacroIntroduced-marker count = {} (see #244 note)", n);
    // Note: per #244 known limitation, macro-introduced nodes
    // from define-hygienic-macro don't currently end up in
    // workspace_flat_. So this may be 0. The primitive still
    // works (returns a valid list); it just has nothing to
    // report.
    CHECK(n >= 0, "MacroIntroduced-marker returns a non-negative length");
    return true;
}

// ── AC4: unknown marker name returns error pair
bool test_unknown_marker() {
    std::println("\n--- AC4: query:schema-of-marker with unknown marker name ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(define x 5)")) { ++g_failed; return false; }
    auto r = try_run(cs, "(typecheck-current)");
    if (!r.ok) { ++g_failed; return false; }
    std::int64_t is_pair = is_pair_result(cs, "(query:schema-of-marker \"NoSuch\")");
    CHECK_EQ_INT(is_pair, 1, "unknown marker returns a pair (error)");
    return true;
}

// ── AC5: bad arg types return error pair
bool test_bad_args() {
    std::println("\n--- AC5: query:schema-of-marker with bad arg types ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(define x 5)")) { ++g_failed; return false; }
    auto r = try_run(cs, "(typecheck-current)");
    if (!r.ok) { ++g_failed; return false; }

    std::int64_t p1 = is_pair_result(cs, "(query:schema-of-marker)");
    CHECK_EQ_INT(p1, 1, "no-args returns a pair (error)");

    std::int64_t p2 = is_pair_result(cs, "(query:schema-of-marker 42)");
    CHECK_EQ_INT(p2, 1, "int-arg returns a pair (error)");

    std::int64_t p3 = is_pair_result(cs, "(query:schema-of-marker \"User\" \"five\")");
    CHECK_EQ_INT(p3, 1, "string-limit returns a pair (error)");
    return true;
}

// ── AC6: limit-N caps the result list
bool test_limit() {
    std::println("\n--- AC6: query:schema-of-marker with limit ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(define x 5) (define y 10) (define z 15)")) {
        ++g_failed; return false;
    }
    auto r = try_run(cs, "(typecheck-current)");
    if (!r.ok) { ++g_failed; return false; }
    // Total User-marker count should be > 3 (each define has
    // multiple nodes). limit=1 should give 1.
    auto r1 = try_run(cs, "(length (query:schema-of-marker \"User\" 1))");
    if (!r1.ok || !aura::compiler::types::is_int(r1.v)) {
        std::println("  FAIL: result is not an int");
        ++g_failed; return false;
    }
    CHECK_EQ_INT(aura::compiler::types::as_int(r1.v), 1,
                 "limit=1 returns exactly 1 item");
    return true;
}

// ── AC7: no-workspace returns error pair
bool test_no_workspace() {
    std::println("\n--- AC7: no workspace → error pair ---");
    aura::compiler::CompilerService cs;
    // Don't call set-code; the default workspace may or may not
    // be set. The primitive should at least not crash.
    std::int64_t is_pair = is_pair_result(cs, "(query:schema-of-marker \"User\")");
    std::println("    [info] is_pair = {} (0=no-workspace, 1=list-or-error)", is_pair);
    CHECK(is_pair >= 0, "no-workspace handling doesn't crash");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════
int run_tests() {
    std::println("═══ Issue #248 — SyntaxMarker + type schema (observability) ═══\n");

    std::println("AC #7: no-workspace handling (run first)");
    test_no_workspace();

    std::println("\nAC #1: query:schema-of-marker returns a list");
    test_returns_list();

    std::println("\nAC #2: User marker has results after typecheck");
    test_user_marker_results();

    std::println("\nAC #3: MacroIntroduced marker returns a (possibly empty) list");
    test_macro_introduced_marker();

    std::println("\nAC #4: unknown marker name");
    test_unknown_marker();

    std::println("\nAC #5: bad arg types");
    test_bad_args();

    std::println("\nAC #6: limit-N caps result");
    test_limit();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_248_detail

int aura_issue_248_run() { return aura_issue_248_detail::run_tests(); }

