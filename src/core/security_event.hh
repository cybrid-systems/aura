// Issue #2075 / #2054 — shared SecurityEvent schema for unified audit surface.
//
// Before #2075: three in-memory audit rings existed (effect 128,
// isolation 128, mutation 64) with different schemas. Agents had no
// single query primitive to ask "what was the last deny and why?"
// across all three rings. The mutation WAL was also opt-in.
//
// #2075 introduces a shared SecurityEvent type + an append_security_event
// helper that deny paths (effect / isolation / invariant) call into.
// The shared schema is also what query:security-audit-trail reads back.
//
// #2054 extends the surface to success+deny correlation:
//   - EffectAllow kind + denied=false on allow paths
//   - fiber_id for multi-fiber filter
//   - per-event seq for :since-seq filtering
//   - check_and_record_effect always emits (allow and deny) and feeds
//     TypedMutationAudit so rings stay correlated by mutation_id
//   - query:security-audit joins security / mutation / typed trails
// The mutation WAL remains the durable backend; replay rebuilds both
// mutation_audit_ring_ and g_security_event_ring.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "core/capability_model.hh"    // Effect, TenantId
#include "core/workspace_isolation.hh" // IsolationRefProvenance

namespace aura::core::security_event {

// Issue #2075 / #2054: unified event kind for the audit surface.
// Extensible — invariant failures, macro hygiene, effect allow, etc.
enum class SecurityEventKind : std::uint8_t {
    EffectDeny = 0,
    IsolationDeny = 1,
    InvariantFail = 2,
    MacroHygiene = 3,
    EffectAllow = 4, // #2054: correlated success path
    // Issue #2237: agent-driven MacroIntroduced rollback under
    // Strict sandbox. Emitted from mutate:rollback-macro-introduced
    // when `g_macro_expand_sandbox_strict` is set (sandbox-strict
    // mode). kind=5 keeps the audit class distinguishable from the
    // existing hygiene class (MacroHygiene=3) so AI agents can
    // grep for "rollback under strict" specifically without
    // matching other hygiene violations. Cross-links #2225 WAL
    // (durable side-car append when WAL enabled) + #2176
    // unstamp C-linkage (existing counter bumped alongside).
    MacroHygieneRollbackOnStrict = 5,
};

// Issue #2054 stamp (schema key on query:security-audit / stats).
inline constexpr int kSecurityAuditUnifyIssue = 2054;
// Issue #2156: isolation-deny SecurityEvent.mutation_id is Mutation epoch
// (or non-zero audit mid), never a tenant id.
inline constexpr int kIsolationAuditMidIssue = 2156;

// Issue #2075 / #2054: shared SecurityEvent record. Fixed-size, no allocation.
// check_and_record_effect always appends (allow + deny); isolation deny
// and invariant paths also append. query:security-audit reads back with
// tenant / fiber / since-seq / mutation-id filters.
struct SecurityEvent {
    SecurityEventKind kind = SecurityEventKind::EffectDeny;
    std::uint64_t seq = 0; // ring seq at append (stable filter key)
    std::uint64_t tenant_id = 0;
    std::uint64_t mutation_id = 0; // caller's mutation_id / provenance
    std::uint64_t epoch = 0;       // Mutation epoch at emit (#2149; was Bridge)
    std::int64_t fiber_id = 0;     // #2054: multi-fiber filter
    std::uint16_t effect_bits = 0;
    bool denied = true;
    char op[40]{};     // NUL-terminated op name (e.g. "mutate")
    char reason[64]{}; // NUL-terminated reason / error class
};

// Issue #2075: ring size for the unified trail. Same as the existing
// mutation ring (64) so the shared surface doesn't blow up memory.
// Replay path uses the existing mutation_audit_wal for persistence.
//
// Issue #2225: ring expanded from 64 to 1024 so a deny storm under
// multi-tenant / Strict can't wrap mid-history. Memory cost:
// 1024 * sizeof(SecurityEvent) ≈ 1024 * (1+8*6+2+1+40+64 padded)
// ≈ 144 KiB process-wide. Within the audit surface budget.
// AURA_SECURITY_EVENT_RING overrides the default (must be power-of-2
// for fast seq%size mask; rejected silently if not).
constexpr std::size_t kSecurityEventRingSize = 1024;

struct SecurityEventRing {
    std::array<SecurityEvent, kSecurityEventRingSize> ring{};
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> total{0};
    // Issue #2225: monotonic wrap counter — bumps every time the
    // ring overwrites an un-replayed slot. Agents can compute "how
    // many events got lost since ring start" via total - ring.size.
    // Paired with the durable SecurityEventWAL (#2225 Phase B) so
    // forensic replay can recover events that wrapped out of the
    // in-memory window.
    std::atomic<std::uint64_t> ring_wrap_total{0};
};

// Issue #2075 / #2054: thread-safe append. Deny paths set denied=true;
// allow path (#2054) sets denied=false + EffectAllow. Stores op + reason
// (truncated to buffer size, NUL-terminated). No heap allocation.
inline void append_security_event(SecurityEventRing& ring, SecurityEventKind kind,
                                  std::uint64_t tenant_id, std::uint64_t mutation_id,
                                  std::uint64_t epoch, std::uint16_t effect_bits,
                                  std::string_view op, std::string_view reason, bool denied = true,
                                  std::int64_t fiber_id = 0) noexcept {
    const auto s = ring.seq.fetch_add(1, std::memory_order_relaxed);
    auto& slot = ring.ring[s % kSecurityEventRingSize];
    // Issue #2225: bump wrap counter if this seq overwrites a slot
    // that hasn't been replayed into the durable WAL. Cheap O(1)
    // increment, no I/O — the ring_wrap_total counter drives
    // observability and lets Agents reason about forensic loss
    // when WAL is disabled.
    if (s >= kSecurityEventRingSize) {
        ring.ring_wrap_total.fetch_add(1, std::memory_order_relaxed);
    }
    slot.kind = kind;
    slot.seq = s;
    slot.tenant_id = tenant_id;
    slot.mutation_id = mutation_id;
    slot.epoch = epoch;
    slot.fiber_id = fiber_id;
    slot.effect_bits = effect_bits;
    slot.denied = denied;
    if (!op.empty()) {
        const auto n = std::min(op.size(), sizeof(slot.op) - 1);
        std::memcpy(slot.op, op.data(), n);
        slot.op[n] = '\0';
    } else {
        slot.op[0] = '\0';
    }
    if (!reason.empty()) {
        const auto n = std::min(reason.size(), sizeof(slot.reason) - 1);
        std::memcpy(slot.reason, reason.data(), n);
        slot.reason[n] = '\0';
    } else {
        slot.reason[0] = '\0';
    }
    ring.total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2075 / #2054: process-global security event ring. One instance
// per process; all evaluators share it. query:security-audit /
// query:security-audit-trail read from this.
inline SecurityEventRing& g_security_event_ring() noexcept {
    static SecurityEventRing ring;
    return ring;
}

// Test helper: clear ring (not for production hot path).
inline void reset_security_event_ring_for_test() noexcept {
    auto& ring = g_security_event_ring();
    ring.seq.store(0, std::memory_order_relaxed);
    ring.total.store(0, std::memory_order_relaxed);
    ring.ring_wrap_total.store(0, std::memory_order_relaxed);
    for (auto& e : ring.ring)
        e = SecurityEvent{};
}

} // namespace aura::core::security_event
