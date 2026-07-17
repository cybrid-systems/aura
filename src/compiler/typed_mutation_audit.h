// typed_mutation_audit.h — Issue #1589 / #1216: production TypedMutationAuditPass.
// Thread-safe strategy gate, contextual event capture, in-memory ring trail.
// Header form so serve/evaluator/tests can include without module churn.

#ifndef AURA_COMPILER_TYPED_MUTATION_AUDIT_H
#define AURA_COMPILER_TYPED_MUTATION_AUDIT_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace aura::compiler::typed_audit {

inline constexpr int kTypedMutationAuditPassPhase = 2; // #1589 production
inline constexpr int kTypedMutationAuditIssue = 1589;
inline constexpr std::size_t kTypedMutationAuditTrailSize = 256;
inline constexpr std::size_t kAuditNameCap = 48;

enum class AuditStrategy : std::uint8_t {
    Off = 0,
    Sampled = 1,
    Full = 2,
};

enum class MutationKind : std::uint8_t {
    Unknown = 0,
    Structural = 1,
    ReplaceType = 2,
    ReplaceValue = 3,
    RecordPatch = 4,
    Other = 5,
};

enum class AuditOutcome : std::uint8_t {
    Success = 0,
    Rollback = 1,
    Error = 2,
};

struct TypedMutationAuditEvent {
    std::uint64_t mutation_id = 0;
    std::uint64_t seq = 0;
    char name[kAuditNameCap]{};
    MutationKind kind = MutationKind::Unknown;
    std::uint64_t before_epoch = 0;
    std::uint64_t after_epoch = 0;
    AuditOutcome outcome = AuditOutcome::Success;
    std::uint32_t target_node = 0;
    std::uint32_t nodes_changed = 0;
    std::int64_t fiber_id = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint32_t affected_ref_count = 0;
};

// Process-wide atomics (thread-safe).
struct TypedMutationAuditCounters {
    std::atomic<std::uint64_t> audits_considered{0};
    std::atomic<std::uint64_t> samples_skipped{0};
    std::atomic<std::uint64_t> contextual_total{0}; // AC: typed_mutation_audit_contextual_total
    std::atomic<std::uint64_t> trail_writes{0};
    std::atomic<std::uint64_t> rollbacks{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint32_t> strategy{static_cast<std::uint32_t>(AuditStrategy::Sampled)};
    std::atomic<std::uint32_t> sample_ratio{4}; // every Nth id when Sampled (N>=1)
    std::atomic<std::uint64_t> trail_seq{0};
};

inline TypedMutationAuditCounters g_typed_mutation_audit_counters{};

// Ring buffer protected by mutex (writers on mutation path; readers via query).
struct TypedMutationAuditTrail {
    std::mutex mu;
    TypedMutationAuditEvent ring[kTypedMutationAuditTrailSize]{};
};

inline TypedMutationAuditTrail& g_trail() {
    static TypedMutationAuditTrail t;
    return t;
}

[[nodiscard]] inline AuditStrategy get_strategy() noexcept {
    return static_cast<AuditStrategy>(
        g_typed_mutation_audit_counters.strategy.load(std::memory_order_relaxed));
}

inline void set_strategy(AuditStrategy s) noexcept {
    g_typed_mutation_audit_counters.strategy.store(static_cast<std::uint32_t>(s),
                                                   std::memory_order_relaxed);
}

inline void set_sample_ratio(std::uint32_t n) noexcept {
    if (n == 0)
        n = 1;
    g_typed_mutation_audit_counters.sample_ratio.store(n, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint32_t get_sample_ratio() noexcept {
    return g_typed_mutation_audit_counters.sample_ratio.load(std::memory_order_relaxed);
}

// Thread-safe Full / Sampled / Off gate.
// Sampled: audit when mutation_id % sample_ratio == 0.
[[nodiscard]] inline bool should_audit(std::uint64_t mutation_id) noexcept {
    g_typed_mutation_audit_counters.audits_considered.fetch_add(1, std::memory_order_relaxed);
    const auto s = get_strategy();
    if (s == AuditStrategy::Off)
        return false;
    if (s == AuditStrategy::Full)
        return true;
    const auto ratio = get_sample_ratio();
    if (ratio <= 1)
        return true;
    if ((mutation_id % ratio) != 0) {
        g_typed_mutation_audit_counters.samples_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

[[nodiscard]] inline MutationKind classify_kind(std::string_view op) noexcept {
    if (op.empty())
        return MutationKind::Unknown;
    if (op.find("replace-type") != std::string_view::npos || op == "replace-type")
        return MutationKind::ReplaceType;
    if (op.find("replace-value") != std::string_view::npos || op == "replace-value")
        return MutationKind::ReplaceValue;
    if (op.find("record-patch") != std::string_view::npos || op == "record-patch")
        return MutationKind::RecordPatch;
    if (op == "structural" || op.find("mutate") != std::string_view::npos)
        return MutationKind::Structural;
    return MutationKind::Other;
}

inline void capture_audit_event(std::uint64_t mutation_id, std::string_view name, MutationKind kind,
                                std::uint64_t before_epoch, std::uint64_t after_epoch,
                                AuditOutcome outcome, std::uint32_t target_node = 0,
                                std::uint32_t nodes_changed = 0, std::int64_t fiber_id = 0,
                                std::uint32_t affected_ref_count = 0) noexcept {
    if (!should_audit(mutation_id))
        return;

    TypedMutationAuditEvent ev{};
    ev.mutation_id = mutation_id;
    const auto seq =
        g_typed_mutation_audit_counters.trail_seq.fetch_add(1, std::memory_order_relaxed);
    ev.seq = seq;
    const auto n = name.size() < (kAuditNameCap - 1) ? name.size() : (kAuditNameCap - 1);
    if (n > 0)
        std::memcpy(ev.name, name.data(), n);
    ev.name[n] = '\0';
    ev.kind = kind;
    ev.before_epoch = before_epoch;
    ev.after_epoch = after_epoch;
    ev.outcome = outcome;
    ev.target_node = target_node;
    ev.nodes_changed = nodes_changed;
    ev.fiber_id = fiber_id;
    ev.timestamp_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    ev.affected_ref_count = affected_ref_count;

    {
        std::lock_guard lock(g_trail().mu);
        g_trail().ring[seq % kTypedMutationAuditTrailSize] = ev;
    }

    g_typed_mutation_audit_counters.contextual_total.fetch_add(1, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.trail_writes.fetch_add(1, std::memory_order_relaxed);
    if (outcome == AuditOutcome::Rollback)
        g_typed_mutation_audit_counters.rollbacks.fetch_add(1, std::memory_order_relaxed);
    if (outcome == AuditOutcome::Error)
        g_typed_mutation_audit_counters.errors.fetch_add(1, std::memory_order_relaxed);
}

// Convenience for mutation boundary integration.
inline void record_boundary_outcome(std::uint64_t mutation_id, std::string_view op,
                                    std::uint64_t before_epoch, std::uint64_t after_epoch,
                                    bool success, std::uint32_t target_node = 0,
                                    std::uint32_t nodes_changed = 0,
                                    std::int64_t fiber_id = 0) noexcept {
    capture_audit_event(mutation_id, op, classify_kind(op), before_epoch, after_epoch,
                        success ? AuditOutcome::Success : AuditOutcome::Rollback, target_node,
                        nodes_changed, fiber_id, nodes_changed > 0 ? 1u : 0u);
}

[[nodiscard]] inline std::uint64_t trail_size() noexcept {
    const auto writes =
        g_typed_mutation_audit_counters.trail_writes.load(std::memory_order_relaxed);
    return writes < kTypedMutationAuditTrailSize ? writes : kTypedMutationAuditTrailSize;
}

[[nodiscard]] inline std::uint64_t trail_seq() noexcept {
    return g_typed_mutation_audit_counters.trail_seq.load(std::memory_order_relaxed);
}

// Copy latest event (seq-1) or empty if none.
[[nodiscard]] inline bool trail_latest(TypedMutationAuditEvent& out) noexcept {
    const auto seq = trail_seq();
    if (seq == 0)
        return false;
    std::lock_guard lock(g_trail().mu);
    out = g_trail().ring[(seq - 1) % kTypedMutationAuditTrailSize];
    return true;
}

// Copy event by absolute seq if still in ring window.
[[nodiscard]] inline bool trail_at_seq(std::uint64_t seq, TypedMutationAuditEvent& out) noexcept {
    const auto head = trail_seq();
    if (head == 0 || seq >= head)
        return false;
    if (head > kTypedMutationAuditTrailSize && seq < head - kTypedMutationAuditTrailSize)
        return false;
    std::lock_guard lock(g_trail().mu);
    out = g_trail().ring[seq % kTypedMutationAuditTrailSize];
    return out.seq == seq;
}

inline void snapshot_global(std::uint64_t& considered, std::uint64_t& skipped,
                            std::uint64_t& contextual, std::uint64_t& trail_sz,
                            std::uint64_t& rollbacks, std::uint64_t& errors,
                            std::uint32_t& strategy, std::uint32_t& sample_ratio) noexcept {
    considered = g_typed_mutation_audit_counters.audits_considered.load(std::memory_order_relaxed);
    skipped = g_typed_mutation_audit_counters.samples_skipped.load(std::memory_order_relaxed);
    contextual = g_typed_mutation_audit_counters.contextual_total.load(std::memory_order_relaxed);
    trail_sz = trail_size();
    rollbacks = g_typed_mutation_audit_counters.rollbacks.load(std::memory_order_relaxed);
    errors = g_typed_mutation_audit_counters.errors.load(std::memory_order_relaxed);
    strategy = g_typed_mutation_audit_counters.strategy.load(std::memory_order_relaxed);
    sample_ratio = g_typed_mutation_audit_counters.sample_ratio.load(std::memory_order_relaxed);
}

// Test helper: reset counters + trail (not for production hot path).
inline void reset_for_test() noexcept {
    g_typed_mutation_audit_counters.audits_considered.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.samples_skipped.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.contextual_total.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.trail_writes.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.rollbacks.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.errors.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.trail_seq.store(0, std::memory_order_relaxed);
    set_strategy(AuditStrategy::Sampled);
    set_sample_ratio(4);
    std::lock_guard lock(g_trail().mu);
    for (auto& e : g_trail().ring)
        e = TypedMutationAuditEvent{};
}

} // namespace aura::compiler::typed_audit

#endif // AURA_COMPILER_TYPED_MUTATION_AUDIT_H
