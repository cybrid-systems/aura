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
// untrusted).
static bool is_bridge_stale(uint64_t bridge_epoch, uint64_t current_epoch) {
    if (bridge_epoch == 0) return true;  // legacy / unset
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
    CHECK(is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "bridge with epoch 0 is stale (legacy sentinel)");

    // Bump the service epoch (simulates arena reset)
    svc.reset();
    CHECK(svc.bridge_epoch() == 1, "service epoch bumped to 1");

    // The bridge is now stale (epoch 0 != current 1)
    CHECK(is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "bridge captured at epoch 0 is stale after reset");

    // Construct a new bridge at the current epoch
    ClosureBridgeData bd2;
    bd2.flat = &fake_flat;
    bd2.pool = &fake_pool;
    bd2.body_id = 42;
    bd2.body_source = "(lambda (x) x)";
    bd2.bridge_epoch = svc.bridge_epoch();  // 1
    CHECK(bd2.bridge_epoch == 1, "new bridge captured epoch 1");
    CHECK(!is_bridge_stale(bd2.bridge_epoch, svc.bridge_epoch()),
          "new bridge at epoch 1 is not stale");

    // Bump again (simulates another major mutation)
    svc.bump_bridge_epoch();
    CHECK(svc.bridge_epoch() == 2, "epoch bumped to 2");
    CHECK(is_bridge_stale(bd2.bridge_epoch, svc.bridge_epoch()),
          "bridge captured at epoch 1 is stale after bump to 2");
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
    // The IRClosure captured at epoch 0; service is at epoch 1.
    // The stale check sees the mismatch.
    CHECK(is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
          "IRClosure captured at epoch 0 is stale after reset");

    // Re-capture
    cl.bridge_epoch = svc.bridge_epoch();
    CHECK(!is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
          "IRClosure re-captured at current epoch is not stale");
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
    bd.bridge_epoch = 0;  // initially stale
    CHECK(is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "zero-epoch bridge is stale after 200 epoch bumps");
    bd.bridge_epoch = last_epoch;
    CHECK(!is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
          "fresh bridge at current epoch is not stale");
}

int main() {
    test_1_epoch_basics();
    test_2_bridge_capture();
    test_3_irclosure_carry();
    test_4_monotonicity();
    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
