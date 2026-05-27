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

// TLS: current running fiber (nullptr = scheduler context)
thread_local Fiber* g_current_fiber = nullptr;
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
    // uc_link: set dynamically when first scheduled (scheduler->main_ctx_)
    // The trampoline yields back to scheduler at the end instead.
    ctx_.uc_link = nullptr;

    // makecontext needs function pointer with (int, int) signature on all POSIX
    // We pass fiber id (high 32 / low 32) so trampoline can look up the fiber
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

// ── Resume — scheduler → fiber ────────────────────────

void Fiber::resume() {
    auto prev = g_current_fiber;
    g_current_fiber = this;
    state_ = FiberState::Ready;
    if (::swapcontext(&g_scheduler->main_ctx(), &ctx_) == -1) {
        std::fprintf(stderr, "fiber[%lu]: resume swapcontext failed: %s\n",
                     (unsigned long)id_, std::strerror(errno));
    }
    g_current_fiber = prev;
}

// ── Yield — fiber → scheduler ─────────────────────────

void Fiber::yield() {
    if (g_current_fiber && g_scheduler) {
        auto& ctx = g_current_fiber->ctx_;
        if (::swapcontext(&ctx, &g_scheduler->main_ctx()) == -1) {
            std::fprintf(stderr, "fiber: yield swapcontext failed: %s\n",
                         std::strerror(errno));
        }
    }
}

// ── Trampoline — first entry point when fiber starts ──

void Fiber::trampoline(uint32_t /*high*/, uint32_t /*low*/) {
    if (g_current_fiber) {
        g_current_fiber->state_ = FiberState::Ready;
        g_current_fiber->func_();
        // Function returned — fiber is done
        g_current_fiber->state_ = FiberState::Done;
    }
    // Return from trampoline → uc_link = nullptr → behaves like yield back to scheduler
    // Actually, uc_link = nullptr means thread exits when makecontext returns.
    // We need to yield back to scheduler instead.
    Fiber::yield();
}

} // namespace aura::serve
