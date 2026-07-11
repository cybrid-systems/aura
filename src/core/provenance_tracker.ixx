// provenance_tracker.ixx — Issues #1180/#1185/#1191 Phase 1: StableNodeRef scaffold.

module;

export module aura.core.provenance_tracker;

import std;

export namespace aura::core::provenance {

inline constexpr int kProvenanceTrackerPhase = 1;

using NodeId = std::uint32_t;
using Gen = std::uint32_t;
using LayerId = std::uint32_t;
using MutationId = std::uint64_t;
using TenantId = std::uint64_t;

struct StableNodeRef {
    NodeId id = 0;
    Gen gen = 0;
    LayerId layer = 0;
    TenantId tenant_id = 0;
    MutationId source_mutation_id = 0;
    std::uint64_t provenance_epoch = 0;
};

struct ProvenanceTracker {
    std::uint64_t records = 0;
    std::uint64_t validations = 0;
    std::uint64_t dirty_propagations = 0;

    void record_mutation(const StableNodeRef& /*ref*/) noexcept { ++records; }

    [[nodiscard]] bool validate_provenance(const StableNodeRef& ref,
                                           MutationId source) const noexcept {
        return ref.source_mutation_id == 0 || ref.source_mutation_id == source;
    }

    void propagate_dirty_effects(const StableNodeRef& /*ref*/) noexcept { ++dirty_propagations; }
};

inline ProvenanceTracker g_provenance_tracker{};

} // namespace aura::core::provenance
