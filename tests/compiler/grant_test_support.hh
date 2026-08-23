// Test-only: non-zero grant provenance under Restricted/Strict (#3090).
#pragma once

#include "core/capability_model.hh"
#include "core/workspace_epoch.hh"

[[nodiscard]] inline aura::core::capability::EffectProvenance
aura_test_grant_prov(std::uint64_t mid = 0, std::uint32_t fiber_id = 0) noexcept {
    auto me = aura::core::current_mutation_epoch();
    if (me == 0) {
        aura::core::bump_mutation_epoch(1);
        me = aura::core::current_mutation_epoch();
    }
    return aura::core::capability::make_grant_provenance(mid != 0 ? mid : me, true, 0, fiber_id);
}
