// serve/fiber.cpp — Stackful fiber implementation
#include "fiber.h"
#include "scheduler.h"

#include <sys/mman.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <system_error>

namespace aura::serve {

std::atomic<uint64_t> Fiber::next_id_{1};

// TLS: current running fiber (nullptr = worker loop context)
thread_local Fiber* g_current_fiber = nullptr;
// TLS: current worker's dispatch loop context
thread_local WorkerContext* g_worker_ctx = nullptr;

Scheduler* g_scheduler = nullptr;

// ── Constructor ───────────────────────────────────────

Fiber::Fiber(Func func, size_t stack_size)
    : id_(next_id_++), stack_size_(stack_size), func_(std::move(func)) {

    // 1. Allocate stack via mmap with guard page
    // Guard page is the first page (PROT_NONE)
    size_t guard_size = 4096;
    size_t alloc_size = guard_size + stack_size_;

    void* base = ::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        throw std::system_error(errno, std::generic_category(), "fiber mmap stack");

    // Guard page at the bottom (to catch stack underflow from overflow)
    ::mprotect(base, guard_size, PROT_NONE);
    stack_ = static_cast<char*>(base) + guard_size;  // usable starts after guard

    // 2. Create eventfd
    eventfd_ = ::eventfd(0, EFD_NONBLOCK);
    if (eventfd_ == -1) {
        ::munmap(base, alloc_size);
        throw std::system_error(errno, std::generic_category(), "fiber eventfd");
    }

    // 3. Initialize ucontext
    if (::getcontext(&ctx_) == -1) {
        ::munmap(base, alloc_size);
        ::close(eventfd_);
        throw std::system_error(errno, std::generic_category(), "fiber getcontext");
    }

    ctx_.uc_stack.ss_sp = stack_;
    ctx_.uc_stack.ss_size = stack_size_;
    ctx_.uc_link = nullptr;

    // makecontext needs function pointer with (int, int) signature on all POSIX
    uint32_t id_high = static_cast<uint32_t>(id_ >> 32);
    uint32_t id_low  = static_cast<uint32_t>(id_ & 0xFFFFFFFF);
    ::makecontext(&ctx_, reinterpret_cast<void(*)()>(&trampoline), 2, id_high, id_low);
}

// ── Destructor ───────────────────────────────────────

Fiber::~Fiber() {
    if (eventfd_ >= 0) ::close(eventfd_);
    if (stack_) {
        // stack_ = usable start; the mmap base is one guard page before
        auto* base = static_cast<char*>(stack_) - 4096;
        ::munmap(base, 4096 + stack_size_);
    }
}

// ── Resume — worker → fiber ───────────────────────────
// Called from a WorkerThread's dispatch loop.
// Saves the worker's loop context into g_worker_ctx->uctx,
// then swaps to the fiber's context.
// When the fiber yields (or finishes), control returns here.

void Fiber::resume() {
    auto* wctx = g_worker_ctx;
    if (!wctx) {
        std::fprintf(stderr, "fiber[%lu]: resume called with no worker context\n",
                     (unsigned long)id_);
        return;
    }

    auto prev = g_current_fiber;
    g_current_fiber = this;
    state_.store(FiberState::Running, std::memory_order_release);

    // Swap from worker's loop context to fiber's context
    if (::swapcontext(&wctx->uctx, &ctx_) == -1) {
        std::fprintf(stderr, "fiber[%lu]: resume swapcontext failed: %s\n",
                     (unsigned long)id_, std::strerror(errno));
    }

    g_current_fiber = prev;
}

// ── Yield — fiber → worker ────────────────────────────
// Static: called from within a fiber's execution.
// Swaps back to g_worker_ctx (the current worker's dispatch loop).
// After this, the fiber is suspended. The worker's loop will
// re-enqueue or wait depending on the fiber's state.

void Fiber::yield() {
    auto* wctx = g_worker_ctx;
    if (!wctx) {
        std::fprintf(stderr, "fiber: yield called with no worker context\n");
        return;
    }

    auto* fb = g_current_fiber;
    if (!fb) return;

    // Swap from fiber's context back to worker's loop context
    if (::swapcontext(&fb->ctx_, &wctx->uctx) == -1) {
        std::fprintf(stderr, "fiber: yield swapcontext failed: %s\n",
                     std::strerror(errno));
    }
}

// ── Trampoline — first entry point when fiber starts ──

void Fiber::trampoline(uint32_t /*high*/, uint32_t /*low*/) {
    if (g_current_fiber) {
        g_current_fiber->set_state(FiberState::Running);
        g_current_fiber->func_();
        // Function returned — fiber is done
        g_current_fiber->set_state(FiberState::Done);
    }
    // Yield back to worker's loop context
    Fiber::yield();
}

} // namespace aura::serve
