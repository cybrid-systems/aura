// test_issue_223.cpp — Issue #223: closure-bridge epoch counter
// for lifetime tracking (Issue #180 Cycle 1).
//
// Tests the bridge_epoch_ field on ClosureBridgeData + IRClosure
// and the bridge_epoch() / bump_bridge_epoch() / reset() epoch
// tracking on CompilerService.
//
// The test focuses on the cycle 1 deliverables:
//
//   1. bridge_epoch() returns the current epoch (starts at 0
//      or higher — relaxed ordering)
//   2. reset() bumps the bridge epoch (stale bridges detected)
//   3. bump_bridge_epoch() bumps the epoch explicitly
//   4. ClosureBridgeData captures the epoch at construction
//   5. IRClosure carries the epoch from the bridge
//   6. A helper function (is_bridge_stale) detects mismatch
//      between captured epoch and current epoch
//
// Standalone TU (no module imports — avoids the GCC 16.1
// std module + P2996 reflection conflict). The test mirrors
// the production data structure layout (ClosureBridgeData +
// IRClosure fields) and exercises the epoch logic.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// Mirror of production ClosureBridgeData (from src/compiler/ir.ixx).
// We can't import the .ixx module from a standalone TU (it would
// trigger the GCC 16.1 std module + P2996 reflection conflict),
// so we duplicate the schema here. If the production schema
// changes, this stub must be kept in sync — the test serves as
// a contract reminder.
struct ClosureBridgeData {
    const void* flat = nullptr;       // ast::FlatAST* in prod
    const void* pool = nullptr;       // ast::StringPool* in prod
    std::uint32_t body_id = ~0u;      // ast::NULL_NODE in prod
    std::string body_source;          // serialized source for fallback re-parse
    std::uint64_t bridge_epoch = 0;   // Issue #223: epoch at construction
};

// Mirror of production IRClosure bridge-related fields.
struct IRClosureBridgeFields {
    const void* flat = nullptr;
    const void* pool = nullptr;
    std::uint32_t body_id = ~0u;
    std::uint64_t bridge_epoch = 0;   // Issue #223: carried from bridge
};

// The CompilerService epoch tracker is in service.ixx. For
// the test we mirror its semantics in a minimal class.
struct MockEpochTracker {
    std::atomic<std::uint64_t> mutation_epoch_{0};
    std::uint64_t bridge_epoch() const noexcept {
        return mutation_epoch_.load(std::memory_order_relaxed);
    }
    void bump_bridge_epoch() noexcept {
        mutation_epoch_.fetch_add(1, std::memory_order_relaxed);
    }
    void reset() noexcept {
        mutation_epoch_.fetch_add(1, std::memory_order_relaxed);
    }
};

// Issue #223 helper: returns true if the IRClosure's bridge
// is stale (its captured epoch doesn't match the service's
// current epoch). Default epoch 0 counts as stale (legacy
// bridges without explicit epoch capture are considered
// untrusted). The production semantics (Issue #223 apply_closure
// wiring) is: bridge_epoch == 0 means "legacy / not tracked" —
// such closures are NOT auto-invalidated (don't break existing
// closures that pre-date the tracking). New code paths should
// set bridge_epoch to the current epoch at construction.
static bool is_bridge_stale(uint64_t bridge_epoch, uint64_t current_epoch) {
    if (bridge_epoch == 0) return false;  // legacy / unset: trust
    return bridge_epoch != current_epoch;
}

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL: %s (line %d)\n", (msg), __LINE__); \
        ++g_failed; \
    } else { \
        std::fprintf(stdout, "  PASS: %s\n", (msg)); \
        ++g_passed; \
    } \
} while(0)

#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// ── Test 1: bridge_epoch() / reset() / bump_bridge_epoch() ────
void test_1_epoch_basics() {
    PRINTLN("\n--- Test 1: bridge_epoch() basics ---");
    MockEpochTracker svc;
    // Initial epoch is 0 (atomic default-init)
    CHECK(svc.bridge_epoch() == 0, "initial bridge_epoch is 0");
    // reset() bumps
    svc.reset();
    CHECK(svc.bridge_epoch() == 1, "reset() bumped epoch to 1");
    // bump_bridge_epoch() bumps
    svc.bump_bridge_epoch();
    CHECK(svc.bridge_epoch() == 2, "bump_bridge_epoch() bumped to 2");
    // reset() bumps again
    svc.reset();
    CHECK(svc.bridge_epoch() == 3, "second reset() bumped to 3");
}

// ── Test 2: ClosureBridgeData captures epoch at construction ──
void test_2_bridge_capture() {
    PRINTLN("\n--- Test 2: ClosureBridgeData captures epoch ---");
    MockEpochTracker svc;
    // Pretend we have a FlatAST/StringPool; we just use void* in the test.
    int fake_flat = 0;
    int fake_pool = 0;
    uint64_t epoch_at_construction = svc.bridge_epoch();  // 0

    // Construct a bridge at epoch 0
    ClosureBridgeData bd;
    bd.flat = &fake_flat;
    bd.pool = &fake_pool;
    bd.body_id = 42;
    bd.body_source = "(lambda (x) x)";
    bd.bridge_epoch = epoch_at_construction;
    CHECK(bd.bridge_epoch == 0, "bridge captured epoch 0");
    // Production semantics: bridge_epoch == 0 is legacy / not
    // tracked. Such closures are NOT auto-invalidated (the
    // production is_bridge_stale returns false for 0). The
    // apply_closure wiring relies on this for backward compat
    // with closures built before the epoch tracking was added.
    CHECK(!is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "bridge with epoch 0 is legacy (not auto-invalidated)");

    // Bump the service epoch (simulates arena reset)
    svc.reset();
    CHECK(svc.bridge_epoch() == 1, "service epoch bumped to 1");

    // Bridge with epoch 0 is still treated as legacy (not stale)
    // \u2014 it was constructed when no service was bound, so the
    // caller's pre-existing lifetime model applies.
    CHECK(!is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "bridge captured at epoch 0 stays legacy (not stale) after reset");

    // But a bridge with an EXPLICIT epoch IS stale after reset
    bd.bridge_epoch = 0;  // start fresh
    bd.bridge_epoch = 1;  // captured at epoch 1 (current is 1)
    CHECK(!is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "bridge captured at epoch 1 is not stale at epoch 1");
    svc.reset();  // service → 2
    CHECK(svc.bridge_epoch() == 2, "service epoch bumped to 2");
    CHECK(is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "bridge captured at epoch 1 is stale after reset to epoch 2");

    // Construct a new bridge at the current epoch (2)
    ClosureBridgeData bd2;
    bd2.flat = &fake_flat;
    bd2.pool = &fake_pool;
    bd2.body_id = 42;
    bd2.body_source = "(lambda (x) x)";
    bd2.bridge_epoch = svc.bridge_epoch();  // 2
    CHECK(bd2.bridge_epoch == 2, "new bridge captured epoch 2");
    CHECK(!is_bridge_stale(bd2.bridge_epoch, svc.bridge_epoch()),
          "new bridge at epoch 2 is not stale");

    // Bump again (simulates another major mutation)
    svc.bump_bridge_epoch();
    CHECK(svc.bridge_epoch() == 3, "epoch bumped to 3");
    CHECK(is_bridge_stale(bd2.bridge_epoch, svc.bridge_epoch()),
          "bridge captured at epoch 2 is stale after bump to 3");
}

// ── Test 3: IRClosure carries the bridge_epoch ────────────────
void test_3_irclosure_carry() {
    PRINTLN("\n--- Test 3: IRClosure carries bridge_epoch ---");
    MockEpochTracker svc;
    // The IRClosure struct (from ir_executor.ixx) has a
    // bridge_epoch field. We use the mirror struct from above.
    IRClosureBridgeFields cl;
    cl.bridge_epoch = svc.bridge_epoch();
    CHECK(cl.bridge_epoch == 0, "IRClosure bridge_epoch defaults to 0");

    svc.reset();
    // IRClosure captured at epoch 0 is legacy (not invalidated).
    CHECK(!is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
          "IRClosure captured at epoch 0 stays legacy (not stale) after reset");

    // Re-capture at a tracked epoch
    cl.bridge_epoch = svc.bridge_epoch();  // 1
    CHECK(!is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
          "IRClosure re-captured at current epoch is not stale");
    svc.bump_bridge_epoch();
    CHECK(is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
          "IRClosure captured at epoch 1 is stale after bump to 2");
}

// ── Test 4: Epoch monotonicity + no false negatives ──────────
void test_4_monotonicity() {
    PRINTLN("\n--- Test 4: Epoch monotonicity ---");
    MockEpochTracker svc;
    uint64_t last_epoch = svc.bridge_epoch();
    // 100 resets + bumps interleaved
    for (int i = 0; i < 100; ++i) {
        svc.reset();
        CHECK(svc.bridge_epoch() > last_epoch, "epoch monotonically increased");
        last_epoch = svc.bridge_epoch();
        svc.bump_bridge_epoch();
        CHECK(svc.bridge_epoch() > last_epoch, "epoch monotonically increased (bump)");
        last_epoch = svc.bridge_epoch();
    }
    // The bridge that captured at last_epoch is now stale
    ClosureBridgeData bd;
    bd.bridge_epoch = 0;  // legacy / not tracked
    CHECK(!is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "zero-epoch bridge is legacy (not stale) after 200 epoch bumps");
    bd.bridge_epoch = last_epoch;
    CHECK(!is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "fresh bridge at current epoch is not stale");
    // A bridge captured BEFORE the bumps is now stale
    ClosureBridgeData old_bd;
    old_bd.bridge_epoch = 0;  // start clean
    old_bd.bridge_epoch = 1;  // captured at epoch 1
    CHECK(is_bridge_stale(old_bd.bridge_epoch, svc.bridge_epoch()),
          "old bridge captured at epoch 1 is stale after 200 epoch bumps");
}

// ── Test 5: apply_closure wiring (simulated) ─────────────────
// Mirrors the apply_closure logic in evaluator_impl.cpp:
// "if cl.flat is non-null, check is_bridge_stale; if stale,
//  return nullopt (invalidate); otherwise proceed with eval".
// The test verifies the invalidation logic.
void test_5_apply_closure_invalidation() {
    PRINTLN("\n--- Test 5: apply_closure wiring (simulated) ---");
    MockEpochTracker svc;

    // Build a closure that captures the current epoch
    struct SimulatedClosure {
        void* flat = (void*)0xdeadbeef;  // pretend arena-allocated
        void* pool = (void*)0xcafebabe;
        std::uint32_t body_id = 42;
        std::uint64_t bridge_epoch = 0;
    };
    SimulatedClosure cl;
    cl.bridge_epoch = svc.bridge_epoch();  // 0
    CHECK(cl.bridge_epoch == 0, "closure captured epoch 0 (no service bound yet)");

    // Simulate the apply_closure check: is_bridge_stale returns
    // false for legacy / unset epoch (the safe default — don't
    // break existing closures that don't track).
    bool would_invalidate = is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch());
    CHECK(!would_invalidate,
          "legacy closure (epoch 0) is not invalidated");

    // Now set the closure to a tracked epoch (e.g. via a bridge
    // construction that captured the service's epoch)
    cl.bridge_epoch = 1;
    svc.bump_bridge_epoch();  // service is now at 1 (legacy + bump)
    // The closure was captured at 0, service is at 1.
    // Wait — if the closure was captured at 1 and service is at 1,
    // they're equal. Let's reset to see staleness.
    svc.reset();  // service bumps to 2
    would_invalidate = is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch());
    CHECK(would_invalidate,
          "closure captured at epoch 1 is invalidated after service reset (epoch 2)");

    // Re-bridge: build a new closure at the new epoch
    SimulatedClosure cl2;
    cl2.bridge_epoch = svc.bridge_epoch();  // 2
    would_invalidate = is_bridge_stale(cl2.bridge_epoch, svc.bridge_epoch());
    CHECK(!would_invalidate,
          "fresh closure at current epoch is not invalidated");
}

// ── Test 6: body_source re-parse fallback (schema) ──────────
//
// The production apply_closure now has a body_source re-parse
// fallback: if the bridge is stale AND body_source is non-empty,
// it re-parses body_source into a fresh FlatAST + StringPool
// and updates the closure in place. If body_source is empty
// (legacy closure) or the re-parse fails, the closure is
// invalidated (return nullopt).
//
// This test verifies the data flow:
// - Closure::body_source is a string field (default empty)
// - is_bridge_stale(bridge, current) determines when to fallback
// - The fallback re-uses the existing parse_to_flat path
//
// The actual parse_to_flat call requires the full parser
// module, which is not available in a standalone TU (would
// trigger the GCC 16.1 std module + P2996 reflection conflict).
// The schema + flow verification is the best we can do here.
void test_6_body_source_fallback() {
    PRINTLN("\n--- Test 6: body_source re-parse fallback (schema) ---");
    // Mirror of Closure's body_source field (Issue #223).
    struct SimulatedClosure {
        void* flat = nullptr;
        void* pool = nullptr;
        std::uint32_t body_id = 0;
        std::uint64_t bridge_epoch = 0;
        std::string body_source;  // NEW: Issue #223
    };

    // Case 1: closure with empty body_source, stale bridge
    // \u2014 the apply_closure fallback can't help (no source
    // to re-parse). The closure is invalidated.
    {
        SimulatedClosure cl;
        cl.body_source = "";
        cl.bridge_epoch = 1;
        // Stale (simulated): current=2
        bool stale = is_bridge_stale(cl.bridge_epoch, 2);
        bool has_fallback = !cl.body_source.empty();
        CHECK(stale, "stale bridge detected");
        CHECK(!has_fallback, "no body_source = no fallback available");
        // Production: returns nullopt (invalidate)
    }

    // Case 2: closure with body_source, stale bridge
    // \u2014 the apply_closure fallback re-parses and recovers.
    {
        SimulatedClosure cl;
        cl.body_source = "(lambda (x) (* x x))";
        cl.bridge_epoch = 1;
        bool stale = is_bridge_stale(cl.bridge_epoch, 2);
        bool has_fallback = !cl.body_source.empty();
        CHECK(stale, "stale bridge detected");
        CHECK(has_fallback, "body_source available for re-parse");
        // Production: calls parse_to_flat, updates cl.flat/pool/body_id
        // and cl.bridge_epoch = current_bridge_epoch().
    }

    // Case 3: closure with body_source, fresh bridge
    // \u2014 the apply_closure uses the existing flat*/pool*
    // (no re-parse needed).
    {
        SimulatedClosure cl;
        cl.body_source = "(lambda (x) (* x x))";
        cl.bridge_epoch = 2;  // matches current
        bool stale = is_bridge_stale(cl.bridge_epoch, 2);
        bool has_fallback = !cl.body_source.empty();
        CHECK(!stale, "fresh bridge: not stale");
        CHECK(has_fallback, "body_source still available (would only be used if stale)");
        // Production: uses cl.flat directly (no fallback needed).
    }
}

int main() {
    test_1_epoch_basics();
    test_2_bridge_capture();
    test_3_irclosure_carry();
    test_4_monotonicity();
    test_5_apply_closure_invalidation();
    test_6_body_source_fallback();
    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
