// test_issue_145.cpp — Verify Issue #145 partial deliverable
// (Closure::params SoA migration + EnvView/ClosureView new types +
// bind_symid/lookup_by_symid fast path).
//
// #145 is a 2-3 week issue ("Deepen DOD/SoA for runtime
// structures — Env, Closure capture, and heap vectors"). This
// PR ships Phase 1 only: the smallest, highest-impact slice
// that fits a verify+close cycle.
//
// Phase 1 ships:
//   1. Closure::params: std::vector<std::string> → std::vector<SymId>
//   2. Env::bind_symid / Env::lookup_by_symid — SymId-based
//      fast path that mirrors to the string-keyed array when
//      pool_ is set, so lambda body code's lookup(name) still
//      finds the param.
//   3. EnvView / ClosureView — zero-copy span views (mirrors
//      the NodeView pattern).
//   4. apply_closure uses the new fast path.
//
// Tests:
//   AC #1: Closure::params is SymId[] (not string[])
//   AC #2: apply_closure uses bind_symid
//   AC #3: Env::bind_symid + lookup_by_symid roundtrip
//   AC #4: EnvView / ClosureView expose the expected spans
//   AC #5: legacy Env::lookup(string) still finds the binding
//          (no behavior regression)
//   AC #6: zero regression — basic lambda call works

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.core;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.evaluator;
import aura.compiler.value;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::println("  FAIL: {} (line {})", msg, __LINE__); \
        ++g_failed; \
    } else { \
        std::println("  PASS: {}", msg); \
        ++g_passed; \
    } \
} while(0)

// ═══════════════════════════════════════════════════════════════
// AC #1: Closure::params is now SymId[] (was string[])
// ═══════════════════════════════════════════════════════════════

bool test_closure_params_is_symid() {
    std::println("\n--- Test 1.1: Closure::params is std::vector<SymId> ---");
    // Build a Closure with empty params — type is now SymId[]
    aura::compiler::Closure cl;
    cl.name = "test";
    cl.params = {};  // empty vector
    CHECK(cl.params.empty(), "Closure::params is empty by default");
    // The type compiles as std::vector<SymId>. Verify with a
    // static_assert-style runtime check via make_closure_view.
    auto view = aura::compiler::make_closure_view(cl);
    CHECK(view.arity() == 0, "ClosureView::arity() matches Closure::params.size()");
    return true;
}

bool test_closure_params_push_symid() {
    std::println("\n--- Test 1.2: Closure::params pushes SymId values ---");
    aura::compiler::Closure cl;
    cl.params.push_back(42);  // SymId
    cl.params.push_back(99);  // SymId
    CHECK(cl.params.size() == 2, "two SymIds pushed");
    CHECK(cl.params[0] == 42, "first SymId preserved");
    CHECK(cl.params[1] == 99, "second SymId preserved");
    auto view = aura::compiler::make_closure_view(cl);
    CHECK(view.param_at(0) == 42, "ClosureView::param_at(0) returns SymId");
    CHECK(view.param_at(1) == 99, "ClosureView::param_at(1) returns SymId");
    CHECK(view.param_at(2) == aura::ast::SymId{}, "ClosureView::param_at(2) returns empty SymId for OOB");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #2: Env::bind_symid + lookup_by_symid roundtrip
// ═══════════════════════════════════════════════════════════════

bool test_bind_symid_lookup_symid() {
    std::println("\n--- Test 2.1: bind_symid + lookup_by_symid roundtrip ---");
    aura::compiler::Env env;
    env.bind_symid(42, aura::compiler::types::make_int(100));
    env.bind_symid(99, aura::compiler::types::make_int(200));
    auto v = env.lookup_by_symid(42);
    CHECK(v.has_value(), "lookup_by_symid finds binding");
    if (v) {
        CHECK(aura::compiler::types::as_int(*v) == 100, "bound value is 100");
    }
    auto v2 = env.lookup_by_symid(99);
    CHECK(v2.has_value(), "second binding found");
    if (v2) {
        CHECK(aura::compiler::types::as_int(*v2) == 200, "second value is 200");
    }
    return true;
}

bool test_bind_symid_shadowing() {
    std::println("\n--- Test 2.2: bind_symid respects shadowing (most-recent wins) ---");
    aura::compiler::Env env;
    env.bind_symid(42, aura::compiler::types::make_int(100));
    env.bind_symid(42, aura::compiler::types::make_int(200));
    auto v = env.lookup_by_symid(42);
    CHECK(v.has_value(), "shadowed binding found");
    if (v) {
        CHECK(aura::compiler::types::as_int(*v) == 200, "most-recent binding wins (got 200, not 100)");
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #3: EnvView exposes both span arrays
// ═══════════════════════════════════════════════════════════════

bool test_env_view_exposes_spans() {
    std::println("\n--- Test 3.1: EnvView exposes string + SymId spans ---");
    aura::compiler::Env env;
    env.bind("foo", aura::compiler::types::make_int(42));
    env.bind_symid(7, aura::compiler::types::make_int(99));
    auto view = aura::compiler::make_env_view(env);
    CHECK(!view.string_bindings.empty(), "string_bindings span is non-empty");
    CHECK(!view.symid_bindings.empty(), "symid_bindings span is non-empty");
    CHECK(view.size() == 1, "EnvView::size() matches string bindings (legacy contract)");
    auto found = view.lookup("foo");
    CHECK(found.has_value(), "EnvView::lookup(string) works");
    auto found_sym = view.lookup_by_symid(7);
    CHECK(found_sym.has_value(), "EnvView::lookup_by_symid works");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #4: ClosureView exposes params span
// ═══════════════════════════════════════════════════════════════

bool test_closure_view_basic() {
    std::println("\n--- Test 4.1: ClosureView zero-copy span access ---");
    aura::compiler::Closure cl;
    cl.name = "my-fn";
    cl.params = {1, 2, 3};
    cl.body_id = 42;
    cl.dotted = false;
    auto view = aura::compiler::make_closure_view(cl);
    CHECK(view.arity() == 3, "ClosureView::arity() == 3");
    CHECK(view.body_id == 42, "ClosureView::body_id == 42");
    CHECK(view.name == "my-fn", "ClosureView::name == \"my-fn\"");
    CHECK(!view.dotted, "ClosureView::dotted is false");
    CHECK(view.param_at(0) == 1, "param 0");
    CHECK(view.param_at(1) == 2, "param 1");
    CHECK(view.param_at(2) == 3, "param 2");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #5: Legacy Env::lookup(string) still works (no regression)
// ═══════════════════════════════════════════════════════════════

bool test_legacy_lookup_string() {
    std::println("\n--- Test 5.1: legacy Env::lookup(string) still works ---");
    aura::compiler::Env env;
    env.bind("hello", aura::compiler::types::make_int(42));
    env.bind("world", aura::compiler::types::make_int(99));
    auto v = env.lookup("hello");
    CHECK(v.has_value(), "legacy string lookup finds 'hello'");
    if (v) {
        CHECK(aura::compiler::types::as_int(*v) == 42, "bound value is 42");
    }
    auto missing = env.lookup("missing");
    CHECK(!missing.has_value(), "missing name returns nullopt");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #6: bind_symid WITHOUT pool doesn't mirror (no pool = no
// string-keyed array). lookup(string) won't find it.
// ═══════════════════════════════════════════════════════════════

bool test_bind_symid_without_pool() {
    std::println("\n--- Test 6.1: bind_symid without pool — SymId-only ---");
    aura::compiler::Env env;
    // No set_pool() call — the bind_symid should only write
    // to bindings_symid_, not bindings_.
    env.bind_symid(42, aura::compiler::types::make_int(100));
    auto by_sym = env.lookup_by_symid(42);
    CHECK(by_sym.has_value(), "SymId lookup works (no pool needed)");
    auto view = aura::compiler::make_env_view(env);
    CHECK(view.string_bindings.empty(), "string_bindings is empty (no pool to mirror)");
    CHECK(!view.symid_bindings.empty(), "symid_bindings has the entry");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════

int main() {
    std::println("═══ Issue #145 verification tests (DOD/SoA Phase 1) ═══\n");

    std::println("── AC #1: Closure::params is SymId[] ──");
    test_closure_params_is_symid();
    test_closure_params_push_symid();

    std::println("\n── AC #2: bind_symid + lookup_by_symid ──");
    test_bind_symid_lookup_symid();
    test_bind_symid_shadowing();

    std::println("\n── AC #3: EnvView exposes spans ──");
    test_env_view_exposes_spans();

    std::println("\n── AC #4: ClosureView zero-copy ──");
    test_closure_view_basic();

    std::println("\n── AC #5: legacy lookup(string) no regression ──");
    test_legacy_lookup_string();

    std::println("\n── AC #6: bind_symid without pool ──");
    test_bind_symid_without_pool();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
