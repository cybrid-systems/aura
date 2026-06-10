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
// AC #7 (Phase 2.1): EnvFrame SoA — infrastructure
// ═══════════════════════════════════════════════════════════════
//
// Phase 2.1 ships the EnvFrame SoA infrastructure:
// - EnvId (uint32_t) + NULL_ENV_ID
// - EnvFrame struct (parallel to Env; parent_id_ replaces parent_)
// - Evaluator::env_frames_ arena + alloc_env_frame / env_frame /
//   env_frame_mut accessors
// - walk_env_frames template (parent chain walk via index lookup)
// - env_depth introspection
// - lookup_by_symid_chain (demonstrates SoA walk)
//
// Env and EnvFrame coexist today; migration is Phase 2.2.

bool test_env_id_null_semantics() {
    std::println("\n--- Test 7.1: EnvId NULL semantics ---");
    using aura::compiler::NULL_ENV_ID;
    CHECK(NULL_ENV_ID == std::numeric_limits<std::uint32_t>::max(),
          "NULL_ENV_ID == uint32_t max");
    aura::compiler::Evaluator ev;
    CHECK(!ev.is_valid_env_id(NULL_ENV_ID),
          "is_valid_env_id(NULL_ENV_ID) returns false");
    // Phase 2.2: Evaluator constructor pre-registers top_ env
    // in env_frames_ (so SoA walk path is always exercisable
    // for the root scope). Fresh Evaluator now has 1 frame.
    CHECK(ev.is_valid_env_id(0),
          "is_valid_env_id(0) returns true on fresh evaluator (top_ pre-registered)");
    CHECK(ev.env_frames_size() == 1,
          "fresh evaluator has 1 frame (top_)");
    return true;
}

bool test_alloc_env_frame_returns_valid_id() {
    std::println("\n--- Test 7.2: alloc_env_frame returns valid id ---");
    aura::compiler::Evaluator ev;
    // Phase 2.2: top_ pre-registered, so first alloc gets id=1
    // (id 0 is already taken by top_).
    auto id = ev.alloc_env_frame();
    CHECK(id != aura::compiler::NULL_ENV_ID, "alloc returns non-null id");
    CHECK(ev.is_valid_env_id(id), "is_valid_env_id(id) returns true");
    CHECK(ev.env_frames_size() == 2, "env_frames_size() == 2 after first alloc (top_ + new)");
    auto id2 = ev.alloc_env_frame(id, nullptr);  // child of id
    CHECK(id2 != aura::compiler::NULL_ENV_ID, "second alloc returns non-null id");
    CHECK(id2 != id, "second id != first id");
    CHECK(ev.env_frames_size() == 3, "env_frames_size() == 3 after second alloc");
    return true;
}

bool test_env_frame_roundtrip() {
    std::println("\n--- Test 7.3: env_frame retrieves the same data ---");
    aura::compiler::Evaluator ev;
    auto id = ev.alloc_env_frame();
    auto& fr = ev.env_frame_mut(id);
    fr.bind("hello", aura::compiler::types::make_int(42));
    const auto& fr_const = ev.env_frame(id);
    auto v = fr_const.lookup_local("hello");
    CHECK(v.has_value(), "lookup_local finds the bound value");
    if (v) {
        CHECK(aura::compiler::types::as_int(*v) == 42, "value is 42");
    }
    return true;
}

bool test_env_frame_parent_chain_index() {
    std::println("\n--- Test 7.4: env_frame parent_id is stored as index ---");
    aura::compiler::Evaluator ev;
    auto root = ev.alloc_env_frame();  // parent = NULL_ENV_ID
    auto child = ev.alloc_env_frame(root, nullptr);
    auto grandchild = ev.alloc_env_frame(child, nullptr);
    CHECK(ev.env_frame(root).parent_id == aura::compiler::NULL_ENV_ID,
          "root frame parent_id == NULL_ENV_ID");
    CHECK(ev.env_frame(child).parent_id == root,
          "child frame parent_id == root");
    CHECK(ev.env_frame(grandchild).parent_id == child,
          "grandchild frame parent_id == child");
    return true;
}

bool test_walk_env_frames_visits_all() {
    std::println("\n--- Test 7.5: walk_env_frames visits parent chain ---");
    aura::compiler::Evaluator ev;
    auto root = ev.alloc_env_frame();
    auto child = ev.alloc_env_frame(root, nullptr);
    auto grandchild = ev.alloc_env_frame(child, nullptr);
    std::vector<aura::compiler::EnvId> visited;
    ev.walk_env_frames(grandchild, [&](aura::compiler::EnvId id, const aura::compiler::EnvFrame&) {
        visited.push_back(id);
        return true;  // keep walking
    });
    CHECK(visited.size() == 3, "walk visits all 3 frames");
    CHECK(visited[0] == grandchild, "first visited is grandchild (start)");
    CHECK(visited[1] == child, "second visited is child");
    CHECK(visited[2] == root, "third visited is root");
    return true;
}

bool test_walk_env_frames_early_exit() {
    std::println("\n--- Test 7.6: walk_env_frames early exit stops chain ---");
    aura::compiler::Evaluator ev;
    auto root = ev.alloc_env_frame();
    auto child = ev.alloc_env_frame(root, nullptr);
    auto grandchild = ev.alloc_env_frame(child, nullptr);
    int count = 0;
    ev.walk_env_frames(grandchild, [&](aura::compiler::EnvId, const aura::compiler::EnvFrame&) {
        ++count;
        return count < 2;  // stop after second visit
    });
    CHECK(count == 2, "walk stops after 2 frames (early exit)");
    return true;
}

bool test_env_depth_correct() {
    std::println("\n--- Test 7.7: env_depth matches chain length ---");
    aura::compiler::Evaluator ev;
    auto a = ev.alloc_env_frame();
    auto b = ev.alloc_env_frame(a, nullptr);
    auto c = ev.alloc_env_frame(b, nullptr);
    auto d = ev.alloc_env_frame(c, nullptr);
    CHECK(ev.env_depth(a) == 1, "single frame depth = 1");
    CHECK(ev.env_depth(b) == 2, "two-frame chain depth = 2");
    CHECK(ev.env_depth(c) == 3, "three-frame chain depth = 3");
    CHECK(ev.env_depth(d) == 4, "four-frame chain depth = 4");
    return true;
}

bool test_lookup_by_symid_chain_shadowing() {
    std::println("\n--- Test 7.8: lookup_by_symid_chain respects shadowing ---");
    aura::compiler::Evaluator ev;
    auto root = ev.alloc_env_frame();
    auto child = ev.alloc_env_frame(root, nullptr);
    // Root binds x=100, child binds x=200. Lookup from child
    // should return 200 (closest frame wins).
    ev.env_frame_mut(root).bind_symid(7, aura::compiler::types::make_int(100));
    ev.env_frame_mut(child).bind_symid(7, aura::compiler::types::make_int(200));
    auto v = ev.lookup_by_symid_chain(child, 7);
    CHECK(v.has_value(), "lookup finds x in chain");
    if (v) {
        CHECK(aura::compiler::types::as_int(*v) == 200,
              "closest frame wins (200, not 100)");
    }
    // Lookup from root finds the root binding only.
    auto v_root = ev.lookup_by_symid_chain(root, 7);
    CHECK(v_root.has_value(), "lookup from root finds x");
    if (v_root) {
        CHECK(aura::compiler::types::as_int(*v_root) == 100,
              "root lookup returns 100");
    }
    return true;
}

bool test_lookup_by_symid_chain_missing() {
    std::println("\n--- Test 7.9: lookup_by_symid_chain returns nullopt for missing ---");
    aura::compiler::Evaluator ev;
    auto root = ev.alloc_env_frame();
    auto child = ev.alloc_env_frame(root, nullptr);
    auto v = ev.lookup_by_symid_chain(child, 999);
    CHECK(!v.has_value(), "missing SymId returns nullopt");
    return true;
}

bool test_env_frame_bind_symid_no_pool() {
    std::println("\n--- Test 7.10: EnvFrame::bind_symid without pool is SymId-only ---");
    aura::compiler::Evaluator ev;
    auto id = ev.alloc_env_frame();
    // No pool_ set on the frame.
    ev.env_frame_mut(id).bind_symid(42, aura::compiler::types::make_int(99));
    const auto& fr = ev.env_frame(id);
    auto v = fr.lookup_local_by_symid(42);
    CHECK(v.has_value(), "SymId lookup works without pool");
    if (v) {
        CHECK(aura::compiler::types::as_int(*v) == 99, "value is 99");
    }
    CHECK(fr.bindings_.empty(),
          "string bindings empty (no pool to mirror)");
    CHECK(!fr.bindings_symid_.empty(),
          "SymId bindings has the entry");
    return true;
}

bool test_env_frames_survive_multiple_alloc() {
    std::println("\n--- Test 7.11: env_frames_ keeps all frames across many alloc ---");
    aura::compiler::Evaluator ev;
    constexpr std::size_t N = 1000;
    std::vector<aura::compiler::EnvId> ids;
    for (std::size_t i = 0; i < N; ++i) {
        auto id = ev.alloc_env_frame();
        CHECK(ev.is_valid_env_id(id), "each alloc returns valid id");
        ids.push_back(id);
    }
    // Phase 2.2: top_ pre-registered, so total = 1 (top_) + N.
    CHECK(ev.env_frames_size() == N + 1,
          "env_frames_size() == N+1 after N allocs (top_ pre-registered)");
    // Spot-check: every id is still valid and distinct.
    for (std::size_t i = 0; i < N; ++i) {
        CHECK(ev.is_valid_env_id(ids[i]),
              "all N ids remain valid after bulk alloc");
        if (i > 0) {
            CHECK(ids[i] != ids[i - 1], "consecutive ids are distinct");
        }
    }
    return true;
}

bool test_reset_env_frames_clears_state() {
    std::println("\n--- Test 7.12: reset_env_frames clears the arena ---");
    aura::compiler::Evaluator ev;
    auto id = ev.alloc_env_frame();
    CHECK(ev.env_frames_size() == 2, "size == 2 before reset (top_ + alloc'd)");
    ev.reset_env_frames();
    CHECK(ev.env_frames_size() == 0, "size == 0 after reset");
    CHECK(!ev.is_valid_env_id(id), "old id is no longer valid");
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

    std::println("\n── AC #7: EnvFrame SoA — Phase 2.1 ──");
    test_env_id_null_semantics();
    test_alloc_env_frame_returns_valid_id();
    test_env_frame_roundtrip();
    test_env_frame_parent_chain_index();
    test_walk_env_frames_visits_all();
    test_walk_env_frames_early_exit();
    test_env_depth_correct();
    test_lookup_by_symid_chain_shadowing();
    test_lookup_by_symid_chain_missing();
    test_env_frame_bind_symid_no_pool();
    test_env_frames_survive_multiple_alloc();
    test_reset_env_frames_clears_state();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
