// provenance_tracker.ixx — Issues #1180/#1564: StableNodeRef provenance enforcement.
// Full process-wide metrics + policy live in provenance_tracker.hh.

module;

export module aura.core.provenance_tracker;

import std;

export namespace aura::core::provenance {

inline constexpr int kProvenanceTrackerPhase = 2; // #1564
inline constexpr int kProvenanceTrackerIssue = 1564;

using NodeId = std::uint32_t;
using Gen = std::uint32_t;
using LayerId = std::uint32_t;
using MutationId = std::uint64_t;
using TenantId = std::uint64_t;

enum class AutoRefreshPolicy : std::uint8_t {
    Off = 0,
    AutoRefreshOnBoundary = 1,
    FailOnStale = 2,
};

// Scaffold handle (not FlatAST::StableNodeRef). Production EDSL uses FlatAST refs.
struct StableNodeRef {
    NodeId id = 0;
    Gen gen = 0;
    LayerId layer = 0;
    TenantId tenant_id = 0;
    MutationId source_mutation_id = 0;
    std::uint64_t provenance_epoch = 0;
    std::uint64_t cow_epoch = 0;
    std::uint32_t wrap_epoch = 0;
    std::uint32_t fiber_id = 0;
    bool boundary_pinned = false;
};

struct ProvenanceTracker {
    std::uint64_t records = 0;
    std::uint64_t validations = 0;
    std::uint64_t dirty_propagations = 0;
    AutoRefreshPolicy policy = AutoRefreshPolicy::AutoRefreshOnBoundary;

    void record_mutation(const StableNodeRef& /*ref*/) noexcept { ++records; }

    [[nodiscard]] bool validate_provenance(const StableNodeRef& ref,
                                           MutationId source) const noexcept {
        ++const_cast<ProvenanceTracker*>(this)->validations;
        return ref.source_mutation_id == 0 || ref.source_mutation_id == source;
    }

    // Full provenance check without FlatAST (demo / scaffold).
    [[nodiscard]] bool is_valid_full(const StableNodeRef& ref, Gen current_gen,
                                     std::uint32_t current_wrap, std::uint64_t current_cow,
                                     std::uint32_t current_fiber,
                                     TenantId current_tenant) const noexcept {
        if (ref.id == 0)
            return false;
        if (ref.gen != 0 && ref.gen != current_gen)
            return false;
        if (ref.wrap_epoch != 0 && ref.wrap_epoch != current_wrap)
            return false;
        if (!ref.boundary_pinned && ref.cow_epoch != 0 && ref.cow_epoch != current_cow)
            return false;
        if (ref.fiber_id != 0 && current_fiber != 0 && ref.fiber_id != current_fiber)
            return false;
        if (ref.tenant_id != 0 && current_tenant != 0 && ref.tenant_id != current_tenant)
            return false;
        return true;
    }

    void propagate_dirty_effects(const StableNodeRef& /*ref*/) noexcept { ++dirty_propagations; }
};

inline ProvenanceTracker g_provenance_tracker{};

} // namespace aura::core::provenance
