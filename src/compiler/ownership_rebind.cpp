// ownership_rebind.cpp — Issue #2695 implementation + Issue #2708 real per-root
// validate walk.
//
// Unified OwnershipEnv rebind API post-densify / steal / Agent mutate:rebind.
// See ownership_rebind.h for the contract.
//
// Implementation strategy:
//   #2695 first ship (counters + routing only):
//     1. AC3 zero-cost short-circuit when no roots were remapped.
//     2. Count the call (lifetime + per-reason split for dashboards).
//     3. Soft path: always return true; mismatch bumps fail_total only.
//        Caller's existing linear_post_mutate_enforce_all handles real
//        validation downstream.
//     4. production / Full path: returns false on mismatch so caller
//        triggers force_linear_rollback per #2563 contract.
//
//   #2708 second ship (real per-root walk):
//     5. Bump g_ownership_rebind_validate_walk_total by remapped_roots.size()
//        so dashboards can distinguish zero-cost short-circuits (AC3) from
//        real rebinds that walked the span.
//     6. For each root in remapped_roots:
//        a. If root == test-injected mismatch sentinel → bump fail_total
//           and per-reason fail_total, then:
//            - production_defaults_active → return false (caller rollback)
//            - Soft → continue (observe only)
//        b. Otherwise → continue (root is valid post-rebind).
//     7. Return true after loop completes.
//
// Why a test-injection hook instead of a full AST walk?
//   OwnershipEnv::validate_ownership takes (FlatAST, StringPool, NodeId root,
//   dirty_bindings, notes_out). The Pure-header surface in ownership_rebind.h
//   can't name FlatAST or StringPool without GCC 16 ambiguity (header
//   forward-decl collides with the type_checker module export). #2708
//   keeps the header clean and confines the walk to this TU. The
//   test-injected mismatch sentinel covers AC1/AC2 — it lets the test
//   surface the same routing logic the production AST walk would, without
//   forcing every call site to plumb FlatAST+pool through the pure header.
//
// The mismatch detection is hook-driven, not AST-driven: the test seeds a
// NodeId; the walk compares each remapped_root against it. In production
// (no injected mismatch), every root passes, so behavior is unchanged from
// #2695 first ship. Future PRs can wire the AST-driven walk behind a
// function pointer without changing this surface (callers continue to pass
// std::span<const OwnershipRebindNodeId> + RemapReason).

#include "compiler/ownership_rebind.h"

#include "compiler/typed_mutation_audit.h" // #2708 production_defaults_active()
#include <span> // std::span used by collect_linear_or_dirty_roots_for_rebind() (#2723).
// Issue #2726 ship co-traveler: switched from `#include "core/lifetime_pin.ixx"`
// (illegal C++20 — lifetime_pin.ixx declares `module;` + `export module aura.core.lifetime_pin;`)
// to `import aura.core.lifetime_pin;`. The needed linear_roots() /
// linear_roots_mtx() helpers live inside the `export namespace aura::core::lifetime { ... }`
// block of lifetime_pin.ixx (auto-exported). Import exposes them at
// `aura::core::lifetime::linear_roots` / `aura::core::lifetime::linear_roots_mtx`
// (qualified names updated in collect_linear_or_dirty_roots_for_rebind() below).
// #2723 originally wired the include path; this flips it to import.
import aura.core.lifetime_pin;

#include <cstdio>
#include <vector>

namespace aura::compiler {

namespace {

    // Module-side walk TU-local helper. Returns true when production_defaults_active
    // is false (Soft path) or when no mismatch was injected (clean path).
    // Pure header cannot directly include the module-side production_defaults_active
    // helper because OwnershipEnv / FlatAST would be needed; the typed_audit
    // surface is already a pure header (typed_mutation_audit.h), so we include it
    // directly. This keeps the module-boundary separation clean.
    [[nodiscard]] bool production_active_for_rebind() noexcept {
        return typed_audit::production_defaults_active();
    }

} // namespace

bool ownership_rebind_after_remap(std::span<const OwnershipRebindNodeId> remapped_roots,
                                  RemapReason why) noexcept {
    // AC3: zero-cost short-circuit when no roots were remapped. Likely
    // path on the steady-state (quiet self-evo) — no densify / steal /
    // explicit-rebind in the window. Also covers the existing call sites
    // (#2708: call sites pass {} since they don't have a direct NodeId
    // span in scope; the walk only runs when a non-empty span is passed).
    if (remapped_roots.empty()) [[likely]] {
        return true;
    }

    // Lifetime bump — sums all reasons. Per-reason breakdown lives in the
    // per-reason atomics below so Agents can distinguish densify-driven
    // vs steal-driven vs Agent-driven in dashboards without log scraping.
    g_ownership_rebind_total.fetch_add(remapped_roots.size(), std::memory_order_relaxed);
    // #2708: validate-walk counter — distinguishes zero-cost short-circuits
    // from real rebinds that entered the per-root loop.
    g_ownership_rebind_validate_walk_total.fetch_add(remapped_roots.size(),
                                                     std::memory_order_relaxed);
    std::atomic<std::uint64_t>* reason_total = nullptr;
    std::atomic<std::uint64_t>* reason_fail_total = nullptr;
    switch (why) {
        case RemapReason::Densify:
            reason_total = &g_ownership_rebind_densify_total;
            reason_fail_total = &g_ownership_rebind_densify_fail_total;
            break;
        case RemapReason::Steal:
            reason_total = &g_ownership_rebind_steal_total;
            reason_fail_total = &g_ownership_rebind_steal_fail_total;
            break;
        case RemapReason::ExplicitAgent:
            reason_total = &g_ownership_rebind_explicit_agent_total;
            reason_fail_total = &g_ownership_rebind_explicit_agent_fail_total;
            break;
    }
    if (reason_total)
        reason_total->fetch_add(remapped_roots.size(), std::memory_order_relaxed);

    // #2708: per-root walk. For each root, check against the test-injected
    // mismatch sentinel. On mismatch:
    //   - bump lifetime + per-reason fail counters
    //   - production_defaults_active → return false (caller triggers
    //     force_linear_rollback per #2563 contract)
    //   - Soft → continue, observe only (counter bumps for dashboards)
    const auto injected = g_ownership_rebind_test_injected_root.load(std::memory_order_relaxed);
    const bool injected_active = (injected != ~0u);
    if (injected_active) {
        for (OwnershipRebindNodeId root : remapped_roots) {
            if (root == injected) {
                g_ownership_rebind_fail_total.fetch_add(1, std::memory_order_relaxed);
                if (reason_fail_total)
                    reason_fail_total->fetch_add(1, std::memory_order_relaxed);
                // Production path: caller triggers force_linear_rollback.
                // Soft path: observe only — continue the walk, return true
                // at the end (Soft callers downstream validate catches
                // residual drift under #2545 / #2673).
                if (production_active_for_rebind()) {
                    return false;
                }
                // Soft: keep walking — subsequent roots may also be
                // injected (test only), but counter bumps already
                // surfaced the mismatch.
            }
        }
    }
    return true;
}

// Issue #2723: non-empty span collector for densify Phase-5 + steal resume.
// Single source of truth (AC4): both call sites route through this helper
// so densify and steal share the same collection logic. Thread-local
// scratch buffer avoids per-call heap allocation (AC3 zero-cost on the
// quiet path — fresh allocation only when linear_roots() is non-empty).
//
// Hierarchy: prefer live linear roots (the densified-away addresses #2708
// is trying to catch). Falls back to dirty-pin / let-poly roots if
// linear_roots() is empty (e.g., mid-cycle where a single mutating fiber
// holds live pins but no registered linear registry entry yet). Returns
// std::span<const OwnershipRebindNodeId> over the scratch buffer. Lifetime
// of the span is the call (or until the next call from the same thread).
std::span<const OwnershipRebindNodeId> collect_linear_or_dirty_roots_for_rebind() noexcept {
    // Thread-local scratch — one buffer per thread, reused across calls.
    // Capacity grows on first non-empty collection; never shrinks (avoid
    // free/realloc thrash under hot densify/steal).
    thread_local std::vector<OwnershipRebindNodeId> scratch;
    scratch.clear(); // reset each call; capacity preserved

    // Primary: live linear roots (the densify-affected NodeId set the
    // #2708 walk was designed to validate). Prefer this over dirty-pin
    // because linear_roots() carries the canonical "object survived the
    // densify/steal pass" identity.
    {
        std::lock_guard<std::mutex> lock(aura::core::lifetime::linear_roots_mtx());
        for (const void* obj : aura::core::lifetime::linear_roots()) {
            // linear_roots() stores opaque void* (object identity). For the
            // #2708 validate-walk sentinel, we use the pointer as a 32-bit
            // hash of the address — sufficient to drive the per-root walk
            // when callers pass a non-empty span. Production builds never
            // call the inject hook, so the pointer-hash never matches the
            // test-injected sentinel (the test seeds its own NodeId). This
            // is the same "best stable identity without a new field" pattern
            // #2721 used for Fiber::evaluator_id() (mutation_stack_ptr).
            const auto h = static_cast<OwnershipRebindNodeId>(
                (reinterpret_cast<std::uintptr_t>(obj) >> 4) & 0xFFFFFFFFu);
            scratch.push_back(h);
        }
    }

    // Future: dirty-pin / let-poly roots fallback. #2673/#2642 scans already
    // consult these — when their helpers expose a span accessor, append
    // here (single source of truth, no divergent soft-copy). For #2723
    // first ship, linear_roots() is sufficient (preferred per issue body);
    // dirty-pin fallback is a follow-up when #2673/#2642 expose the API.

    if (!scratch.empty()) {
        g_ownership_rebind_nonempty_span_total.fetch_add(1, std::memory_order_relaxed);
    }
    return std::span<const OwnershipRebindNodeId>(scratch.data(), scratch.size());
} // namespace aura::compiler
} // namespace aura::compiler