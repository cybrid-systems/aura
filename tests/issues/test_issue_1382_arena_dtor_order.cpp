// test_issue_1382_arena_dtor_order.cpp — Issue #1382:
// aura::ast::ASTArena contract test: `run_destructors()` MUST run before
// `resource_.release()` in both `~aura::ast::ASTArena()` and `reset()`.
//
// ## Why this matters
//
// `std::pmr::monotonic_buffer_resource::deallocate()` is a no-op
// per the C++ standard — it never frees upstream-allocated
// chunks. Containers inside arena-allocated objects (e.g.
// FlatAST's 18+ `std::pmr::vector`s) often allocate fallback
// chunks from the upstream on first growth past the arena buffer.
//
// To avoid leaking those upstream chunks, every constructed
// object MUST be destroyed BEFORE the arena's underlying bytes
// are released. The current `~aura::ast::ASTArena()` (src/core/arena.ixx
// ~line 290) and `reset()` (~line 345) both call
// `run_destructors()` BEFORE `resource_.release()`. Order is
// correct today — but it's an implicit contract, not
// enforceable at the type level.
//
// Any future reset path (e.g. the planned pool-backed
// `defrag()` from Issue #300 P1, or `partial_reset(low, high)`)
// that forgets to call `run_destructors()` first will silently
// leak all pmr-vector fallback chunks (or, worse, run
// destructors on stale arena memory after release).
//
// ## What this test verifies
//
// 1. **Destructor counter** (`DtorTracker::count`) — increments
//    each time a tracked object is destroyed. Proves
//    `run_destructors()` ran.
// 2. **Upstream `deallocate` count** (`CountingMR::dealloc_count_`)
//    — proves `monotonic_buffer_resource::release()` ran (and
//    therefore fallback chunks were returned to the upstream).
// 3. **Order** — DtorTracker::count incremented BEFORE the
//    upstream saw its deallocate calls. Detected by recording
//    `dealloc_count_` at the moment each destructor runs.
//
// ## AC
//
// - Pass on Linux/macOS with libc++ and libstdc++.
// - Fail (regression alarm) if `run_destructors()` is moved
//   below `resource_.release()` in either `~aura::ast::ASTArena()` or
//   `reset()`. Specifically: a sentinel destructor that
//   observes the upstream state at the time it runs should
//   see zero deallocations (proving release() hasn't fired
//   yet). If release() runs first, the sentinel sees > 0.

#include "test_harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;

// ── CountingMR — counting memory_resource ──────────────────
//
// Tracks every do_allocate / do_deallocate call. Forwards to
// std::pmr::new_delete_resource() so the underlying allocations
// are real (the test would be meaningless with a null resource).
class CountingMR : public std::pmr::memory_resource {
public:
    std::atomic<std::size_t> alloc_count_{0};
    std::atomic<std::size_t> dealloc_count_{0};
    std::atomic<std::size_t> alloc_bytes_{0};
    std::atomic<std::size_t> dealloc_bytes_{0};

    // Snapshot helper for order verification.
    [[nodiscard]] std::size_t dealloc_snapshot() const noexcept {
        return dealloc_count_.load(std::memory_order_acquire);
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        alloc_count_.fetch_add(1, std::memory_order_relaxed);
        alloc_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        return std::pmr::new_delete_resource()->allocate(bytes, align);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
        dealloc_count_.fetch_add(1, std::memory_order_relaxed);
        dealloc_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        std::pmr::new_delete_resource()->deallocate(p, bytes, align);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }
};

// ── DtorTracker — sentinel object that records its own
//    destruction time relative to upstream deallocations ───
//
// Used to verify that destructors run BEFORE the upstream
// sees deallocate calls. If release() ran first, the sentinel
// would observe dealloc_count_ > 0 at the moment of its
// destruction — that's the regression alarm.
struct DtorTracker {
    static inline std::atomic<int> count{0};
    // Counter snapshot taken AT THE MOMENT of destruction.
    // Should be 0 if run_destructors() runs before
    // resource_.release() (the correct contract).
    static inline std::atomic<std::size_t> dealloc_at_dtor{0};
    static inline std::atomic<const CountingMR*> last_upstream{nullptr};

    CountingMR* upstream = nullptr;

    explicit DtorTracker(CountingMR* up)
        : upstream(up) {}

    ~DtorTracker() {
        count.fetch_add(1, std::memory_order_relaxed);
        if (upstream) {
            // Capture upstream state at the moment of
            // destruction. If release() ran first, this
            // snapshot will be > 0 — that's the regression.
            dealloc_at_dtor.store(upstream->dealloc_snapshot(), std::memory_order_release);
            last_upstream.store(upstream, std::memory_order_release);
        }
    }
};

// ── ContainerWithVector — pmr vector grown past arena
//    buffer to force fallback chunks ──────────────────────
//
// The arena is constructed with a small initial buffer
// (e.g. 4KB). The vector below grows past that, forcing
// `monotonic_buffer_resource` to allocate fallback chunks
// from the upstream (`CountingMR`). After the vector +
// arena go out of scope, those fallback chunks must be
// returned to the upstream via `release()`.
struct ContainerWithVector {
    std::pmr::vector<std::uint64_t> vec;

    explicit ContainerWithVector(std::pmr::polymorphic_allocator<std::byte> alloc)
        : vec(alloc) {}
};

// ── AC1: reset() triggers upstream deallocate AND
//         destructors ran before release() ─────────────────
bool test_reset_runs_destructors_then_release() {
    std::println("\n--- AC1: reset() runs destructors before release() ---");

    auto upstream = std::make_unique<CountingMR>();
    DtorTracker::count.store(0);
    DtorTracker::dealloc_at_dtor.store(0);
    DtorTracker::last_upstream.store(nullptr);

    {
        // 4KB initial buffer — too small for the vector below.
        aura::ast::ASTArena arena(4 * 1024, upstream.get());

        // Create the tracker inside the arena. Its dtor will
        // observe the upstream state at the moment of
        // destruction.
        auto* tracker = arena.create<DtorTracker>(upstream.get());
        CHECK(tracker != nullptr, "tracker created in arena");

        // Create a container with a pmr vector grown past 4KB.
        // Forces the monotonic_buffer_resource to allocate
        // fallback chunks from upstream.
        auto* container = arena.create<ContainerWithVector>(arena.allocator());
        CHECK(container != nullptr, "container created in arena");
        for (std::size_t i = 0; i < 8192; ++i) {
            container->vec.push_back(static_cast<std::uint64_t>(i) * 0xDEADBEEF);
        }
        CHECK(container->vec.size() == 8192, "vector grew to 8192 elements");

        // Verify upstream got at least one allocation (the
        // fallback chunks).
        CHECK(upstream->alloc_count_.load() > 0,
              "fallback allocations happened (upstream saw allocates)");

        // Pre-reset: dtor hasn't fired, upstream hasn't seen
        // deallocates.
        CHECK(DtorTracker::count.load() == 0, "tracker dtor has NOT fired before reset()");
        CHECK(upstream->dealloc_count_.load() == 0,
              "upstream has NOT seen deallocates before reset()");

        arena.reset();

        // Post-reset: dtor fired AND upstream saw deallocates.
        CHECK(DtorTracker::count.load() == 1, "tracker dtor fired during reset()");
        CHECK(upstream->dealloc_count_.load() > 0, "upstream saw deallocates during reset()");

        // ORDER contract: dtor saw 0 deallocations at its
        // moment of destruction. If release() had run first,
        // this would be > 0.
        CHECK(DtorTracker::dealloc_at_dtor.load() == 0,
              "dtor ran BEFORE release() (saw 0 deallocations)");
    }

    return true;
}

// ── AC2: ~aura::ast::ASTArena() triggers upstream deallocate AND
//         destructors ran before release() ─────────────────
bool test_destructor_runs_destructors_then_release() {
    std::println("\n--- AC2: ~aura::ast::ASTArena() runs destructors before release() ---");

    auto upstream = std::make_unique<CountingMR>();
    DtorTracker::count.store(0);
    DtorTracker::dealloc_at_dtor.store(0);
    DtorTracker::last_upstream.store(nullptr);

    {
        aura::ast::ASTArena arena(4 * 1024, upstream.get());

        auto* tracker = arena.create<DtorTracker>(upstream.get());
        CHECK(tracker != nullptr, "tracker created in arena");

        auto* container = arena.create<ContainerWithVector>(arena.allocator());
        for (std::size_t i = 0; i < 8192; ++i) {
            container->vec.push_back(static_cast<std::uint64_t>(i) * 0xCAFEBABE);
        }
        CHECK(container->vec.size() == 8192, "vector grew to 8192 elements");

        CHECK(upstream->alloc_count_.load() > 0, "fallback allocations happened");
        CHECK(DtorTracker::count.load() == 0,
              "tracker dtor has NOT fired before ~aura::ast::ASTArena()");
        CHECK(upstream->dealloc_count_.load() == 0,
              "upstream has NOT seen deallocates before ~aura::ast::ASTArena()");

        // arena goes out of scope here → ~aura::ast::ASTArena() fires.
    }

    CHECK(DtorTracker::count.load() == 1, "tracker dtor fired during ~aura::ast::ASTArena()");
    CHECK(upstream->dealloc_count_.load() > 0,
          "upstream saw deallocates during ~aura::ast::ASTArena()");

    // ORDER contract.
    CHECK(DtorTracker::dealloc_at_dtor.load() == 0,
          "dtor ran BEFORE release() (saw 0 deallocations)");

    return true;
}

// ── AC3: multiple objects + reset → all cleaned up ────────
bool test_multiple_objects_all_destroyed() {
    std::println("\n--- AC3: multiple objects all destroyed ---");

    auto upstream = std::make_unique<CountingMR>();
    DtorTracker::count.store(0);
    DtorTracker::dealloc_at_dtor.store(0);

    constexpr int kN = 16;
    {
        aura::ast::ASTArena arena(4 * 1024, upstream.get());

        std::vector<ContainerWithVector*> containers;
        containers.reserve(kN);
        for (int i = 0; i < kN; ++i) {
            auto* c = arena.create<ContainerWithVector>(arena.allocator());
            // Each container grows past 4KB → triggers its own
            // fallback chunk(s) from upstream.
            for (std::size_t j = 0; j < 4096; ++j) {
                c->vec.push_back(static_cast<std::uint64_t>(j + i * 4096));
            }
            containers.push_back(c);
        }
        // Each container holds a tracker too.
        for (int i = 0; i < kN; ++i) {
            arena.create<DtorTracker>(upstream.get());
        }

        CHECK(upstream->alloc_count_.load() > 0, "upstream saw ≥1 fallback allocations (geometric "
                                                 "growth may pool multiple vectors per chunk)");
        CHECK(DtorTracker::count.load() == 0, "no dtors fired before reset()");

        arena.reset();
    }

    CHECK(DtorTracker::count.load() == kN, "all kN tracker dtors fired");
    CHECK(upstream->dealloc_count_.load() > 0, "upstream saw deallocates from release()");
    // All dtors must have observed 0 deallocations at their
    // moment (proves release() hadn't fired yet).
    CHECK(DtorTracker::dealloc_at_dtor.load() == 0, "every dtor ran BEFORE release()");

    return true;
}

// ── AC4: fallback-allocated bytes are fully returned ──────
//
// Stronger invariant: alloc_bytes_ == dealloc_bytes_ after
// ~aura::ast::ASTArena(). Catches "release() forgot some chunks" bugs.
bool test_bytes_balanced_after_dtor() {
    std::println("\n--- AC4: alloc bytes == dealloc bytes after ~aura::ast::ASTArena() ---");

    auto upstream = std::make_unique<CountingMR>();

    {
        aura::ast::ASTArena arena(4 * 1024, upstream.get());
        auto* c = arena.create<ContainerWithVector>(arena.allocator());
        for (std::size_t i = 0; i < 4096; ++i) {
            c->vec.push_back(static_cast<std::uint64_t>(i));
        }
        // arena goes out of scope.
    }

    auto alloc_b = upstream->alloc_bytes_.load();
    auto dealloc_b = upstream->dealloc_bytes_.load();
    CHECK(alloc_b > 0, "upstream received some allocations");
    CHECK(dealloc_b == alloc_b, "all allocated bytes were returned to upstream (no leak)");

    return true;
}

// ── AC5: default constructor still uses new_delete_resource
//         (regression: ensure optional upstream didn't break
//         the default) ─────────────────────────────────────
bool test_default_upstream_is_new_delete() {
    std::println("\n--- AC5: default constructor unchanged ---");

    // Just ensure the default ctor still works.
    aura::ast::ASTArena arena; // 8MB default, new_delete_resource() upstream
    auto* c = arena.create<ContainerWithVector>(arena.allocator());
    CHECK(c != nullptr, "default ctor + create works");
    c->vec.push_back(42);
    CHECK(c->vec.size() == 1, "vector grows on default arena");
    arena.reset();
    return true;
}

int main() {
    bool ok = true;
    ok &= test_reset_runs_destructors_then_release();
    ok &= test_destructor_runs_destructors_then_release();
    ok &= test_multiple_objects_all_destroyed();
    ok &= test_bytes_balanced_after_dtor();
    ok &= test_default_upstream_is_new_delete();

    // CHECK macro increments aura::test::g_failed on assertion
    // failure; surface it in the exit code (not just `ok`,
    // which only reflects whether test functions returned true).
    int failed = aura::test::g_failed;
    std::println("\n=== Issue #1382 arena dtor order: {} ({} failures) ===",
                 (ok && failed == 0) ? "PASS" : "FAIL", failed);
    return (ok && failed == 0) ? 0 : 1;
}