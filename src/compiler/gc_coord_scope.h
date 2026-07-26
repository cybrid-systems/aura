// gc_coord_scope.h — Issue #2131: unified GC-root pin → cascade → audit
// state machine for invalidate / boundary exit / hot-swap / compact.
//
// Contract (same order on every path):
//   PrePin → Cascade → PostAudit → Released
//
// RAII Scope opens in PrePin. Call enter_cascade() before cascade /
// reemit / remap work. Call after_cascade() after post-cascade root
// audit. Destructor advances to Released and records missing PostAudit
// as a phase violation (debug assert / Strict abort).
//
// Zero new mutexes — sits under existing Mutate / compact interlock /
// boundary order (see lock_order_audit.h). Thread-local nesting is
// supported (outermost owns the metric path tag).
//
// Metrics are process-wide atomics (SlimSurface: extend existing
// query:linear-postmutate-fidelity-stats, no new public *-stats).

#ifndef AURA_COMPILER_GC_COORD_SCOPE_H
#define AURA_COMPILER_GC_COORD_SCOPE_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler::gc_coord {

enum class Phase : std::uint8_t {
    Idle = 0,
    PrePin = 1,
    Cascade = 2,
    PostAudit = 3,
    Released = 4,
};

enum class Path : std::uint8_t {
    Invalidate = 0, // hard invalidate_function
    SoftDirty = 1,  // mark_define_dirty
    Boundary = 2,   // MutationBoundary outermost exit
    HotSwap = 3,    // JIT batch-deopt / hot-swap
    Compact = 4,    // compact_env_frames
    Count = 5,
};

// ── Process-wide observability (#2131) ─────────────────────────────
inline std::atomic<std::uint64_t> scopes_opened_total{0};
inline std::atomic<std::uint64_t> pre_pin_total{0};
inline std::atomic<std::uint64_t> cascade_enter_total{0};
inline std::atomic<std::uint64_t> post_audit_total{0};
inline std::atomic<std::uint64_t> released_total{0};
inline std::atomic<std::uint64_t> phase_violations_total{0};
inline std::atomic<std::uint64_t> missing_post_audit_total{0};
inline std::atomic<std::uint64_t> reverse_order_total{0};
inline std::atomic<std::uint64_t> scopes_by_path[static_cast<std::size_t>(Path::Count)]{};
// When true, reverse-order / illegal transitions abort (Strict).
inline std::atomic<bool> strict_mode{false};

[[nodiscard]] inline std::string_view phase_name(Phase p) noexcept {
    switch (p) {
        case Phase::Idle:
            return "Idle";
        case Phase::PrePin:
            return "PrePin";
        case Phase::Cascade:
            return "Cascade";
        case Phase::PostAudit:
            return "PostAudit";
        case Phase::Released:
            return "Released";
        default:
            return "Unknown";
    }
}

[[nodiscard]] inline std::string_view path_name(Path p) noexcept {
    switch (p) {
        case Path::Invalidate:
            return "invalidate";
        case Path::SoftDirty:
            return "soft_dirty";
        case Path::Boundary:
            return "boundary";
        case Path::HotSwap:
            return "hot_swap";
        case Path::Compact:
            return "compact";
        default:
            return "unknown";
    }
}

// RAII coordination scope — Issue #2131.
class Scope {
public:
    explicit Scope(Path path) noexcept
        : path_(path) {
        parent_ = tls_current();
        tls_current() = this;
        phase_ = Phase::PrePin;
        scopes_opened_total.fetch_add(1, std::memory_order_relaxed);
        pre_pin_total.fetch_add(1, std::memory_order_relaxed);
        const auto pi = static_cast<std::size_t>(path_);
        if (pi < static_cast<std::size_t>(Path::Count))
            scopes_by_path[pi].fetch_add(1, std::memory_order_relaxed);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    ~Scope() noexcept {
        if (!post_audit_done_) {
            missing_post_audit_total.fetch_add(1, std::memory_order_relaxed);
            note_violation_("missing after_cascade before release");
            // Soft-complete: advance so release is still ordered.
            phase_ = Phase::PostAudit;
            post_audit_done_ = true;
            post_audit_total.fetch_add(1, std::memory_order_relaxed);
        }
        phase_ = Phase::Released;
        released_total.fetch_add(1, std::memory_order_relaxed);
        if (tls_current() == this)
            tls_current() = parent_;
    }

    // PrePin → Cascade. Idempotent if already Cascade.
    // Reverse (from PostAudit/Released) is a phase violation.
    void enter_cascade() noexcept {
        if (phase_ == Phase::Cascade)
            return;
        if (phase_ != Phase::PrePin) {
            reverse_order_total.fetch_add(1, std::memory_order_relaxed);
            note_violation_("enter_cascade from non-PrePin");
            return;
        }
        phase_ = Phase::Cascade;
        cascade_enter_total.fetch_add(1, std::memory_order_relaxed);
    }

    // PrePin|Cascade → PostAudit. Idempotent if already PostAudit.
    // Calling after Released is a violation.
    void after_cascade() noexcept {
        if (phase_ == Phase::PostAudit) {
            post_audit_done_ = true;
            return;
        }
        if (phase_ != Phase::PrePin && phase_ != Phase::Cascade) {
            reverse_order_total.fetch_add(1, std::memory_order_relaxed);
            note_violation_("after_cascade from illegal phase");
            return;
        }
        phase_ = Phase::PostAudit;
        post_audit_done_ = true;
        post_audit_total.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Path path() const noexcept { return path_; }
    [[nodiscard]] bool had_violation() const noexcept { return violated_; }
    [[nodiscard]] bool post_audit_done() const noexcept { return post_audit_done_; }

    [[nodiscard]] static Scope* current() noexcept { return tls_current(); }

    // Test seam: open a scope and force reverse enter_cascade after PostAudit.
    static void force_reverse_violation_for_test() noexcept {
        Scope s(Path::Invalidate);
        s.after_cascade();
        s.enter_cascade(); // illegal reverse
    }

private:
    void note_violation_(std::string_view /*why*/) noexcept {
        violated_ = true;
        phase_violations_total.fetch_add(1, std::memory_order_relaxed);
        if (strict_mode.load(std::memory_order_relaxed)) {
#if !defined(NDEBUG)
            // Debug: hard fail so chaos / unit tests catch reverse order.
            std::abort();
#else
            // Release Strict: still abort — production invariant.
            std::abort();
#endif
        }
    }

    [[nodiscard]] static Scope*& tls_current() noexcept {
        thread_local Scope* cur = nullptr;
        return cur;
    }

    Path path_;
    Phase phase_ = Phase::Idle;
    bool post_audit_done_ = false;
    bool violated_ = false;
    Scope* parent_ = nullptr;
};

// If a Scope is active, mark cascade enter (no-op if none / already Cascade).
inline void note_cascade_if_active() noexcept {
    if (auto* s = Scope::current())
        s->enter_cascade();
}

// If a Scope is active, mark PostAudit (no-op if none / already done).
inline void note_post_audit_if_active() noexcept {
    if (auto* s = Scope::current())
        s->after_cascade();
}

} // namespace aura::compiler::gc_coord

#endif // AURA_COMPILER_GC_COORD_SCOPE_H
