// fiber_context.cpp — Issue #4716: fiber-local evaluator execution context.
//
// The pointer the running thread caches is the ONLY thread_local here; the
// Context itself belongs to the fiber and is passed by pointer, so a
// push/pop pair (or a lock frame) can never straddle two workers.

#include "core/fiber_context.hh"

#include <atomic>
#include <new>

namespace aura::core::fiber_ctx {

thread_local Context* t_ctx_cur = nullptr;

namespace {

    // Plain-thread fallback. Created lazily per thread and never destroyed
    // early: a thread exit runs the Context destructor (the cascade-root
    // vector), which is all the cleanup it needs.
    thread_local Context* t_ctx_plain = nullptr;

    // Nested attach stack (a fiber body may drive another fiber).
    thread_local std::vector<Context*> t_ctx_prev;

    std::atomic<std::uint64_t> g_ctx_attach_total{0};
    std::atomic<std::uint64_t> g_ctx_detach_total{0};
    std::atomic<std::uint64_t> g_ctx_plain_total{0};

} // namespace

Context* plain_context() noexcept {
    if (!t_ctx_plain) {
        t_ctx_plain = new (std::nothrow) Context();
        if (t_ctx_plain)
            g_ctx_plain_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (!t_ctx_plain) {
        // Allocation failure: a static per-function fallback keeps the
        // accessor total (never returns null) at the cost of sharing one
        // context across threads for the failure window.
        static Context s_ctx_emergency;
        return &s_ctx_emergency;
    }
    return t_ctx_plain;
}

extern "C" Context* aura_fiber_ctx_create(std::uint64_t owner_fiber) noexcept {
    auto* ctx = new (std::nothrow) Context();
    if (!ctx)
        return nullptr;
    ctx->owner_fiber = owner_fiber;
    return ctx;
}

extern "C" void aura_fiber_ctx_destroy(Context* ctx) noexcept {
    if (!ctx)
        return;
    // Defensive: never leave a dangling cached pointer behind (a fiber
    // destroyed while parked never detaches).
    if (t_ctx_cur == ctx) {
        if (!t_ctx_prev.empty()) {
            t_ctx_cur = t_ctx_prev.back();
            t_ctx_prev.pop_back();
        } else {
            t_ctx_cur = nullptr;
        }
    }
    delete ctx;
}

extern "C" void aura_fiber_ctx_attach(Context* ctx) noexcept {
    t_ctx_prev.push_back(t_ctx_cur);
    t_ctx_cur = ctx;
    g_ctx_attach_total.fetch_add(1, std::memory_order_relaxed);
}

extern "C" Context* aura_fiber_ctx_detach() noexcept {
    Context* ctx = t_ctx_cur;
    if (!t_ctx_prev.empty()) {
        t_ctx_cur = t_ctx_prev.back();
        t_ctx_prev.pop_back();
    } else {
        t_ctx_cur = nullptr;
    }
    if (ctx)
        g_ctx_detach_total.fetch_add(1, std::memory_order_relaxed);
    return ctx;
}

// Issue #4716: the #3617 evaluator-identity mirror (core/gc_hooks.h,
// which keeps its no-STL contract through this C face).
extern "C" void* aura_fiber_eval_identity_get() noexcept {
    return active().eval_identity;
}

extern "C" void aura_fiber_eval_identity_set(void* id) noexcept {
    active().eval_identity = id;
}

extern "C" std::uint64_t aura_fiber_ctx_attach_total() noexcept {
    return g_ctx_attach_total.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_fiber_ctx_detach_total() noexcept {
    return g_ctx_detach_total.load(std::memory_order_relaxed);
}

// Issue #4716 stress/AC probes: mirror lock()/unlock() frame bookkeeping
// on the active context's stack (no mutex), so a test can yield while
// "holding" a frame.
extern "C" void aura_fiber_ctx_treg_push_probe(const void* tag) noexcept {
    Context& c = active();
    if (c.treg_top >= kTregLockFrameCap)
        return;
    c.treg_frames[c.treg_top++] = TregLockFrame{tag, 1, 0};
}

extern "C" void aura_fiber_ctx_treg_pop_probe(const void* tag) noexcept {
    Context& c = active();
    for (int i = c.treg_top - 1; i >= 0; --i) {
        if (c.treg_frames[i].lock == tag) {
            for (int j = i; j < c.treg_top - 1; ++j)
                c.treg_frames[j] = c.treg_frames[j + 1];
            --c.treg_top;
            return;
        }
    }
}

extern "C" int aura_fiber_ctx_treg_depth_probe() noexcept {
    return active().treg_top;
}

extern "C" std::uint64_t aura_fiber_ctx_plain_total() noexcept {
    return g_ctx_plain_total.load(std::memory_order_relaxed);
}

} // namespace aura::core::fiber_ctx
