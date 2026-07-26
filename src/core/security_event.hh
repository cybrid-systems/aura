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
constexpr std::size_t kSecurityEventRingSize = 64;

struct SecurityEventRing {
    std::array<SecurityEvent, kSecurityEventRingSize> ring{};
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> total{0};
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
    for (auto& e : ring.ring)
        e = SecurityEvent{};
}

} // namespace aura::core::security_event
