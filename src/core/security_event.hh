// Issue #2075 — shared SecurityEvent schema for unified audit surface.
//
// Before #2075: three in-memory audit rings existed (effect 128,
// isolation 128, mutation 64) with different schemas. Agents had no
// single query primitive to ask "what was the last deny and why?"
// across all three rings. The mutation WAL was also opt-in.
//
// #2075 introduces a shared SecurityEvent type + an append_security_event
// helper that all deny paths (effect / isolation / invariant) call into.
// The shared schema is also what query:security-audit-trail reads back.
// The mutation WAL replay path already handles round-tripping; #2075
// does NOT change WAL on-disk format — it adds the unified query
// surface in front of the existing rings.
#pragma once

#include <cstdint>

#include "core/capability_model.hh"    // Effect, TenantId
#include "core/workspace_isolation.hh" // IsolationRefProvenance

namespace aura::core::security_event {

// Issue #2075: unified event kind for the audit surface.
// Extensible — invariant failures, macro hygiene, etc. can append
// new kinds without changing the shared schema.
enum class SecurityEventKind : std::uint8_t {
    EffectDeny = 0,
    IsolationDeny = 1,
    InvariantFail = 2,
    MacroHygiene = 3,
};

// Issue #2075: shared SecurityEvent record. Fixed-size, no allocation.
// All three audit rings (effect / isolation / mutation) call
// append_security_event on deny; query:security-audit-trail reads
// back the last N events.
struct SecurityEvent {
    SecurityEventKind kind = SecurityEventKind::EffectDeny;
    std::uint64_t tenant_id = 0;
    std::uint64_t mutation_id = 0; // caller's mutation_id at deny time
    std::uint64_t epoch = 0;       // current_bridge_epoch() at deny time
    std::uint16_t effect_bits = 0;
    bool denied = true; // always true on append (deny only)
    char op[40]{};      // NUL-terminated op name (e.g. "mutate")
    char reason[64]{};  // NUL-terminated reason / error class
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

// Issue #2075: thread-safe append. All deny paths call this.
// Stores op + reason (truncated to buffer size, NUL-terminated).
inline void append_security_event(SecurityEventRing& ring, SecurityEventKind kind,
                                  std::uint64_t tenant_id, std::uint64_t mutation_id,
                                  std::uint64_t epoch, std::uint16_t effect_bits,
                                  std::string_view op, std::string_view reason) noexcept {
    const auto s = ring.seq.fetch_add(1, std::memory_order_relaxed);
    auto& slot = ring.ring[s % kSecurityEventRingSize];
    slot.kind = kind;
    slot.tenant_id = tenant_id;
    slot.mutation_id = mutation_id;
    slot.epoch = epoch;
    slot.effect_bits = effect_bits;
    slot.denied = true;
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

// Issue #2075: process-global security event ring. One instance per
// process; all evaluators share it. query:security-audit-trail reads
// from this.
inline SecurityEventRing& g_security_event_ring() noexcept {
    static SecurityEventRing ring;
    return ring;
}

} // namespace aura::core::security_event
