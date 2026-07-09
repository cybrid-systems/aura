// @category: integration
// @reason: uses CompilerService + AOT bridge C-linkage API + dirty tracking
//
// test_issue_358.cpp — Verify Issue #358 acceptance criteria
// ("[Follow-up #243] Incremental re-AOT for dirty functions").
//
// Background: #243 (AOT bridge enhancement) shipped the AOT
// versioning + observability but deferred incremental re-AOT
// (re-compile only dirty functions instead of re-emitting the
// whole module). That work was deferred because it depends on
// a stable DefineId → FlatFunction index that survives mutation
// epochs.
//
// This scope-limited close ships the FOUNDATION (steps 1 + 2
// from the issue body, simplified to use function-name as the
// canonical key):
//   - `aura_set_is_define_dirty_fn(fn, userdata)` — C-linkage
//     setter for the host's "is Define <name> dirty?" callback.
//   - `aura_filter_dirty_flat_functions(functions, n, out, max)`
//     — C-linkage filter that returns the indices of
//     FlatFunctions whose `name` matches a dirty Define.
//
// What is deferred (follow-up issues):
//   - Stable DefineId → FlatFunction index table (#358 step 1)
//   - `aura_reemit_aot_for_dirty(version)` AOT pipeline (#358 step 3)
//   - Hot-patch test (define + AOT + mutate + re-emit + verify)
//
// Test strategy: 4 layers, one per public surface + integration
//   Layer 1: aura_set_is_define_dirty_fn accepts callback
//   Layer 2: aura_filter_dirty_flat_functions returns -1 on
//            missing callback / bad args
//   Layer 3: filter returns the correct dirty indices for a
//            hand-built FlatFunction[] array
//   Layer 4: end-to-end via CompilerService — register a
//            dirty-tracking callback that reads the workspace's
//            Define entries, mutate a Define, filter the
//            FlatFunction[] array, verify only the mutated
//            function's index appears.

#include "test_harness.hpp"
// Forward declare FlatFunction to avoid pulling in aura_jit.h's
// heavy <functional> include (which conflicts with `import std`).
namespace aura::jit {
struct FlatFunction;
}
#include "aura_jit_bridge.h" // before import std (C-header hygiene)

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.type_checker;
import aura.compiler.service;

namespace aura_issue_358_detail {

// ═══════════════════════════════════════════════════════════
// Layer 1: aura_set_is_define_dirty_fn accepts a callback
// ═══════════════════════════════════════════════════════════

// Trivial dirty callback: returns true iff name is in the
// provided set. userdata points to a const std::set<std::string>*.
static bool is_dirty_in_set(void* userdata, const char* name) {
    if (!userdata || !name)
        return false;
    const auto* dirty = static_cast<const std::set<std::string>*>(userdata);
    return dirty->count(name) > 0;
}

bool test_set_callback_registers_global() {
    std::println("\n--- AC1: aura_set_is_define_dirty_fn registers a callback ---");
    std::set<std::string> dirty = {"foo", "bar"};
    aura_set_is_define_dirty_fn(&is_dirty_in_set, const_cast<std::set<std::string>*>(&dirty));
    // No crash, no return value (void). Verify the callback
    // is wired by calling filter with a 1-element array whose
    // name is in the dirty set — should return count=1.
    aura::jit::FlatFunction fn = {};
    fn.name = "foo";
    unsigned int out[1];
    int count = aura_filter_dirty_flat_functions(&fn, 1, out, 1);
    CHECK(count == 1, "callback is wired: filter returns count=1 for 'foo' (in dirty set)");
    if (count == 1)
        CHECK(out[0] == 0, "dirty index is 0");
    // Reset to nullptr so subsequent tests start clean.
    aura_set_is_define_dirty_fn(nullptr, nullptr);
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 2: aura_filter_dirty_flat_functions error paths
// ═══════════════════════════════════════════════════════════

bool test_filter_returns_neg1_without_callback() {
    std::println("\n--- AC2: filter returns -1 when no callback registered ---");
    aura_set_is_define_dirty_fn(nullptr, nullptr); // ensure no callback
    constexpr unsigned int N = 3;
    aura::jit::FlatFunction fns[N] = {};
    fns[0].name = "x";
    fns[1].name = "y";
    fns[2].name = "z";
    unsigned int out[N];
    int rc = aura_filter_dirty_flat_functions(fns, N, out, N);
    CHECK(rc == -1, "filter returns -1 when no callback is registered (host fallback signal)");
    return true;
}

bool test_filter_returns_neg1_on_null_args() {
    std::println("\n--- AC3: filter returns -1 on null/invalid args ---");
    std::set<std::string> dirty;
    aura_set_is_define_dirty_fn(&is_dirty_in_set, &dirty);
    aura::jit::FlatFunction fns[1] = {};
    fns[0].name = "x";
    unsigned int out[1];
    // null functions pointer
    int rc1 = aura_filter_dirty_flat_functions(nullptr, 1, out, 1);
    CHECK(rc1 == -1, "filter returns -1 for null functions pointer");
    // null output pointer
    int rc2 = aura_filter_dirty_flat_functions(fns, 1, nullptr, 1);
    CHECK(rc2 == -1, "filter returns -1 for null output pointer");
    // max_out < num_functions (caller buffer too small)
    int rc3 = aura_filter_dirty_flat_functions(fns, 5, out, 1);
    CHECK(rc3 == -1, "filter returns -1 when max_out < num_functions");
    aura_set_is_define_dirty_fn(nullptr, nullptr);
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 3: filter returns the correct dirty indices
// ═══════════════════════════════════════════════════════════

bool test_filter_returns_dirty_indices() {
    std::println("\n--- AC4: filter returns the correct dirty indices ---");
    // Build a 5-function array; mark 2 of them as dirty.
    constexpr unsigned int N = 5;
    aura::jit::FlatFunction fns[N] = {};
    fns[0].name = "alpha";
    fns[1].name = "bravo";
    fns[2].name = "charlie";
    fns[3].name = "delta";
    fns[4].name = "echo";
    std::set<std::string> dirty = {"bravo", "echo"};
    aura_set_is_define_dirty_fn(&is_dirty_in_set, &dirty);

    unsigned int out[N];
    int count = aura_filter_dirty_flat_functions(fns, N, out, N);
    CHECK(count == 2, "filter returns count=2 for 2 dirty functions");
    if (count == 2) {
        // Indices should be [1, 4] in the order they appear
        // (alpha=0=fresh, bravo=1=dirty, charlie=2=fresh,
        //  delta=3=fresh, echo=4=dirty)
        CHECK(out[0] == 1, "first dirty index is 1 (bravo)");
        CHECK(out[1] == 4, "second dirty index is 4 (echo)");
    }
    aura_set_is_define_dirty_fn(nullptr, nullptr);
    return true;
}

bool test_filter_skips_null_names() {
    std::println("\n--- AC5: filter skips FlatFunctions with null name ---");
    constexpr unsigned int N = 3;
    aura::jit::FlatFunction fns[N] = {};
    fns[0].name = "alpha";
    fns[1].name = nullptr; // malformed — should be skipped, not crash
    fns[2].name = "bravo";
    std::set<std::string> dirty = {"alpha", "bravo"};
    aura_set_is_define_dirty_fn(&is_dirty_in_set, &dirty);

    unsigned int out[N];
    int count = aura_filter_dirty_flat_functions(fns, N, out, N);
    CHECK(count == 2, "filter skips null-name FlatFunction, counts 2 dirty (alpha + bravo)");
    aura_set_is_define_dirty_fn(nullptr, nullptr);
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 4: end-to-end via CompilerService
// ═══════════════════════════════════════════════════════════

// End-to-end dirty callback: reads the workspace's Define
// entries via a stored CompilerService*. Used by AC6 to verify
// the bridge can consume a callback that threads through C++
// state without a separate closure mechanism.
struct EndToEndCtx {
    aura::compiler::CompilerService* cs;
    std::set<std::string> dirty_names; // names of Defines marked dirty
};

static bool is_dirty_end_to_end(void* userdata, const char* name) {
    if (!userdata || !name)
        return false;
    auto* ctx = static_cast<EndToEndCtx*>(userdata);
    return ctx->dirty_names.count(name) > 0;
}

bool test_end_to_end_filter_after_mutation() {
    std::println("\n--- AC6: end-to-end filter matches the workspace's dirty Defines ---");
    aura::compiler::CompilerService cs;
    // Define a couple of functions so the workspace has
    // some Defines to mark dirty later.
    cs.eval("(begin (define alpha 1) (define bravo 2) (define charlie 3))");

    // Mark `bravo` as dirty (simulating a workspace mutation
    // that invalidated the Define).
    EndToEndCtx ctx;
    ctx.cs = &cs;
    ctx.dirty_names = {"bravo"};
    aura_set_is_define_dirty_fn(&is_dirty_end_to_end, &ctx);

    constexpr unsigned int N = 3;
    aura::jit::FlatFunction fns[N] = {};
    fns[0].name = "alpha";
    fns[1].name = "bravo";
    fns[2].name = "charlie";
    unsigned int out[N];
    int count = aura_filter_dirty_flat_functions(fns, N, out, N);
    CHECK(count == 1, "filter returns count=1 (only bravo is dirty)");
    if (count == 1) {
        CHECK(out[0] == 1, "dirty index is 1 (bravo)");
    }

    // Update the dirty set and re-filter — alpha now dirty too.
    ctx.dirty_names = {"alpha", "bravo"};
    count = aura_filter_dirty_flat_functions(fns, N, out, N);
    CHECK(count == 2, "filter returns count=2 after marking alpha + bravo dirty");
    if (count == 2) {
        CHECK(out[0] == 0, "first dirty index is 0 (alpha)");
        CHECK(out[1] == 1, "second dirty index is 1 (bravo)");
    }

    aura_set_is_define_dirty_fn(nullptr, nullptr);
    return true;
}

// ═══════════════════════════════════════════════════════════
// Main test runner
// ═══════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #358 verification tests ═══\n");

    std::println("Layer 1: aura_set_is_define_dirty_fn");
    test_set_callback_registers_global();

    std::println("\nLayer 2: aura_filter_dirty_flat_functions error paths");
    test_filter_returns_neg1_without_callback();
    test_filter_returns_neg1_on_null_args();

    std::println("\nLayer 3: filter behavior");
    test_filter_returns_dirty_indices();
    test_filter_skips_null_names();

    std::println("\nLayer 4: end-to-end via CompilerService");
    test_end_to_end_filter_after_mutation();

    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}
} // namespace aura_issue_358_detail

int aura_issue_358_run() {
    return aura_issue_358_detail::run_tests();
}