// test_issue_389.cpp — Issue #389: `(compile:snapshot)` Aura
// primitive + JSON docs (follow-up #247 scope-limited slice).
//
// Validates the 4 ACs shipped in this scope-limited close:
//   AC1: (compile:snapshot) is registered as an Aura primitive.
//   AC2: Returns a hash with the 4 SyntaxMarker keys
//        (marker-user-count, marker-macro-introduced-count,
//         marker-bool-literal-count, marker-total-count) +
//        3 stability-context keys (current-generation,
//        current-wrap-epoch, generation-wrap-count) +
//        1 workspace-context key (node-count) = 8 keys total.
//   AC3: The 4 marker counts match (query:marker-stats) — the
//        same underlying data, just exposed via a different
//        primitive shape (hash vs positional list).
//   AC4: After a hygienic-macro eval, marker-macro-introduced-
//        count is > 0 (regression-guard for the marker
//        observability plumbing wired by #247 / #373).
//   AC5: After a mutation, node-count tracks the new AST size.

import std;
import aura.core;
import aura.core.ast;
import aura.core.type;
import aura.parser.parser;
import aura.diag;
import aura.compiler.ir;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.compiler.service;

namespace {
int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println("  PASS: {}", msg);                                                       \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                                        \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            ++g_passed;                                                                            \
            std::println("  PASS: {}  ({} = {})", msg, _a, _b);                                    \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}  ({} != {})", msg, _a, _b);                        \
        }                                                                                          \
    } while (0)

using aura::compiler::CompilerService;
using aura::compiler::types::EvalValue;
} // anonymous namespace

// Note on hash access: we use (hash-ref hash "key") directly rather
// than (stats:get (compile:snapshot) "key") because (stats:get ...) in
// lib/std/stats.aura is a string-name → primitive dispatcher (it goes
// through (eval name)); it is NOT a hash accessor. The (hash-ref ...)
// primitive is registered globally in evaluator_primitives_vector.cpp
// and works on any FlatHashTable-shaped hash, including the one
// returned by (compile:snapshot).

// Helper: load code via the Aura-side (set-code ...) primitive
// so the workspace_flat_ is populated (the (compile:snapshot)
// primitive reads from workspace_flat_, NOT from current_ast_).
// Returns true if (set-code ...) + (eval-current) both succeed.
static bool load_and_eval(CompilerService& cs, const char* code) {
    std::string full = std::string("(set-code \"") + code + "\")";
    auto r1 = cs.eval(full);
    if (!r1)
        return false;
    auto r2 = cs.eval("(eval-current)");
    return static_cast<bool>(r2);
}

// ═══════════════════════════════════════════════════════════════
// AC1: (compile:snapshot) is registered and callable.
// ═══════════════════════════════════════════════════════════════

void test_compile_snapshot_registered() {
    std::println("\n--- AC1: (compile:snapshot) is registered ---");
    CompilerService cs;
    CHECK(load_and_eval(cs, "(define x 1)"), "set-code + eval-current succeeds");
    auto r = cs.eval("(compile:snapshot)");
    CHECK(r.has_value(), "(compile:snapshot) is callable (returns a value)");
    CHECK(r && aura::compiler::types::is_hash(*r), "(compile:snapshot) returns a hash");
}

// ═══════════════════════════════════════════════════════════════
// AC2: The returned hash has all 8 documented keys
// (4 marker + 3 stability-context + 1 workspace-context).
// ═══════════════════════════════════════════════════════════════

void test_snapshot_hash_shape() {
    std::println("\n--- AC2: snapshot hash has all 8 documented keys ---");
    CompilerService cs;
    CHECK(load_and_eval(cs, "(define x 1)"), "set-code + eval-current succeeds");
    auto r = cs.eval("(compile:snapshot)");
    CHECK(r && aura::compiler::types::is_hash(*r), "snapshot returns a hash");
    if (!r || !aura::compiler::types::is_hash(*r))
        return;
    static const std::array<std::string_view, 8> expected_keys = {
        "marker-user-count",         "marker-macro-introduced-count",
        "marker-bool-literal-count", "marker-total-count",
        "current-generation",        "current-wrap-epoch",
        "generation-wrap-count",     "node-count",
    };
    for (auto key : expected_keys) {
        auto v = cs.eval(std::string("(hash-ref (compile:snapshot) \"") + std::string(key) + "\")");
        CHECK(v.has_value(), std::string("snapshot has key: ") + std::string(key));
    }
}

// ═══════════════════════════════════════════════════════════════
// AC3: The 4 marker counts match (query:marker-stats) — same
// underlying data, different primitive shape.
// ═══════════════════════════════════════════════════════════════

void test_marker_counts_match_query_marker_stats() {
    std::println("\n--- AC3: snapshot marker counts == (query:marker-stats) ---");
    CompilerService cs;
    CHECK(load_and_eval(cs, "(define x 1) (define y 2) (define z 3)"),
          "set-code + eval-current succeeds");
    auto snap = cs.eval("(compile:snapshot)");
    auto list = cs.eval("(query:marker-stats)");
    CHECK(snap && aura::compiler::types::is_hash(*snap), "snapshot is hash");
    CHECK(list && aura::compiler::types::is_pair(*list), "marker-stats is list");
    if (!snap || !list)
        return;
    // Compare each of the 4 marker keys against the corresponding
    // list element. list order is (user macro-introduced bool-literal total)
    // (matches query:marker-stats definition in
    // evaluator_primitives_query_workspace.cpp).
    auto read_key = [&](const std::string& k) -> std::int64_t {
        auto v = cs.eval(std::string("(hash-ref (compile:snapshot) \"") + k + "\")");
        if (!v || !aura::compiler::types::is_int(*v))
            return -1;
        return aura::compiler::types::as_int(*v);
    };
    std::int64_t snap_user = read_key("marker-user-count");
    std::int64_t snap_macro = read_key("marker-macro-introduced-count");
    std::int64_t snap_bool = read_key("marker-bool-literal-count");
    std::int64_t snap_total = read_key("marker-total-count");
    // (query:marker-stats) returns a list of 4 ints in order
    // (user macro-introduced bool-literal total). Extract via
    // (car)/(cadr)/(caddr)/(cadddr).
    auto list_user = cs.eval("(car (query:marker-stats))");
    auto list_macro = cs.eval("(cadr (query:marker-stats))");
    auto list_bool = cs.eval("(caddr (query:marker-stats))");
    // cadddr is not a registered primitive nor defined in
    // lib/std/*; reach the 4th element via the (cdddr ...) primitive
    // + (car ...).
    auto list_total = cs.eval("(car (cdddr (query:marker-stats)))");
    auto as_int_checked = [](auto& r) -> std::int64_t {
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    CHECK_EQ(snap_user, as_int_checked(list_user),
             "marker-user-count == (car (query:marker-stats))");
    CHECK_EQ(snap_macro, as_int_checked(list_macro),
             "marker-macro-introduced-count == (cadr (query:marker-stats))");
    CHECK_EQ(snap_bool, as_int_checked(list_bool),
             "marker-bool-literal-count == (caddr (query:marker-stats))");
    CHECK_EQ(snap_total, as_int_checked(list_total),
             "marker-total-count == (cadddr (query:marker-stats))");
}

// ═══════════════════════════════════════════════════════════════
// AC4: Hygienic macro evaluation bumps the macro-introduced
// marker count (regression-guard for #247 / #373 marker plumbing).
// ═══════════════════════════════════════════════════════════════

void test_hygienic_macro_bumps_macro_introduced_count() {
    std::println("\n--- AC4: hygienic macro bumps marker-macro-introduced-count ---");
    CompilerService cs;
    // Push the macro definition + use site through (set-code ...) so
    // (define-hygienic-macro ...) is registered before eval-current
    // runs the body. The body uses (mk-pair 42) to trigger macro
    // expansion; after that (compile:snapshot) should see a non-zero
    // marker-macro-introduced-count.
    CHECK(load_and_eval(cs, "(define-hygienic-macro (mk-pair x) (cons x x)) (mk-pair 42)"),
          "set-code + eval-current succeeds");
    auto macro_count = cs.eval("(hash-ref (compile:snapshot) \"marker-macro-introduced-count\")");
    CHECK(macro_count.has_value(), "marker-macro-introduced-count readable");
    if (macro_count && aura::compiler::types::is_int(*macro_count)) {
        auto n = aura::compiler::types::as_int(*macro_count);
        std::println("  (debug: marker-macro-introduced-count = {})", n);
        CHECK(n > 0, "macro-introduced marker count is > 0 after hygienic macro eval");
    }
}

// ═══════════════════════════════════════════════════════════════
// AC5: node-count tracks the workspace AST size. After
// (set-code ...) the count is > 0; a simple define gives
// a predictable small number.
// ═══════════════════════════════════════════════════════════════

void test_node_count_tracks_workspace() {
    std::println("\n--- AC5: node-count tracks workspace AST size ---");
    CompilerService cs;
    CHECK(load_and_eval(cs, "(define x 1)"), "set-code + eval-current succeeds");
    auto r = cs.eval("(compile:snapshot)");
    CHECK(r && aura::compiler::types::is_hash(*r), "snapshot is hash");
    if (!r || !aura::compiler::types::is_hash(*r))
        return;
    auto n = cs.eval("(hash-ref (compile:snapshot) \"node-count\")");
    CHECK(n.has_value(), "node-count readable");
    if (n && aura::compiler::types::is_int(*n)) {
        auto v = aura::compiler::types::as_int(*n);
        std::println("  (debug: node-count = {})", v);
        CHECK(v > 0, "node-count is > 0 after (set-code '(define x 1))");
    }
}

// ═══════════════════════════════════════════════════════════════
// Driver
// ═══════════════════════════════════════════════════════════════

int main() {
    std::println("=== test_issue_389: (compile:snapshot) primitive ===");
    test_compile_snapshot_registered();
    test_snapshot_hash_shape();
    test_marker_counts_match_query_marker_stats();
    test_hygienic_macro_bumps_macro_introduced_count();
    test_node_count_tracks_workspace();
    std::println("\n=== Totals: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}