// layout_stamp.hh — Issue #2170
// Unified LayoutStamp / generation truth-source API.
//
// One POD snapshot of all cross-subsystem epochs an Agent / FFI /
// AOT emit path needs to make consistent layout-freshness decisions
// against. Strictly non-duplicative to:
//   #1964 cycle 2a–d / #2039 2d — WorkspaceEpoch::{Mutation,Bridge,
//                                Subtree,Wrap,Generation} vocab
//   #2085 — LifetimePin bound to arena generation (per-arena gen)
//   #2091 — AOT env_frame_version + linear_state stamping
//
// Each existing field keeps its storage (process-global atomics in
// workspace_epoch.hh, per-FlatAST generation_, per-Evaluator
// env_generation_ / defuse_version_, per-arena ASTArena::generation_
// + arena_id_). LayoutStamp just composes a snapshot + a single
// freshness check so boundary dtor / live_compact / AOT emit / FFI
// validate pick the SAME current value (instead of each picking a
// different "current" and Agents / FFI restamping the wrong dim).
//
// AC contract (from #2170 body):
//   S1: Force compact -> stamp.arena_gen advances -> pin validate
//       against old stamp fails, against new succeeds.
//   S2: boundary exit publishes stamp consistent with arena + flat
//       gens.
//   S3: concurrent mutate + compact monotonicity (no stamp regression).
//   S4: source-cite single helper used by boundary + compact paths
//       (current_layout_stamp() is the single source of truth).

#ifndef AURA_CORE_LAYOUT_STAMP_HH
#define AURA_CORE_LAYOUT_STAMP_HH

#include <cstdint>
// Note: do NOT include workspace_epoch.hh here. workspace_epoch.hh
// defines inline functions (current_mutation_epoch, etc.) that, when
// pulled in via a module's preamble include, cause ODR redefinition
// errors in other TUs (e.g. evaluator_primitives_mutate.cpp) that
// also include workspace_epoch.hh directly. Capture() method that
// needs current_mutation_epoch() lives in evaluator_mutation_boundary.cpp
// instead — that TU includes workspace_epoch.hh directly without
// re-entering the layout_stamp.hh include chain.

namespace aura::core {

// LayoutStamp — one POD, one freshness check, six fields.
// Trivially copyable so it can be captured by value across the
// mutation boundary + FFI handoff without breaking the FFI flat
// signature (POD = no destructors / no pointers / no vtable).
struct LayoutStamp {
    // arena_id + arena_gen come from the owning ASTArena
    // (per-arena storage, NOT process-global; matches #2085
    // "pin bound to arena generation" lineage).
    std::uint64_t arena_id = 0;
    std::uint64_t arena_gen = 0;
    // flat_gen comes from FlatAST::generation() (per-FlatAST
    // uint16, matches #1964 cycle 2d Generation kind).
    std::uint16_t flat_gen = 0;
    // mutation_epoch is process-global (WorkspaceEpoch::Mutation,
    // #1964 cycle 2b + #2039 2d).
    std::uint64_t mutation_epoch = 0;
    // env_gen comes from Evaluator::env_generation() (EnvFrame
    // truncate / SOAK contract, per-Evaluator).
    std::uint64_t env_gen = 0;
    // defuse_version comes from Evaluator::defuse_version_ (#213
    // cycle 1/2 — boundary publication counter).
    std::uint64_t defuse_version = 0;
    // Issue #2255: shape_version is the ShapeProfiler monotonic
    // generation (bumped on invalidate + on_arena_compact). Adds
    // a 7th field so Fiber resume / JIT deopt / ShapeProfiler
    // version are coherent under steal × hot-update × shape window.
    // Default 0 means "never stamped" — legacy LayoutStamp (pre-#2255)
    // is shape-version-less; the fence treats 0 vs current != 0 as
    // a mismatch only when current != 0 (the cold-start exception).
    std::uint64_t shape_version = 0;
    // Issue #2432: ir_soa_generation is the process-global IR SoA
    // generation fence (advanced on every mark_*_dirty / bump on
    // IRFunctionSoA / IRModuleV2). 8th field closes silent-stale
    // specialized IR under compact×mutate×fiber resume when dirty
    // bits are false but generation advanced (#2111 lineage).
    // Default 0 = never stamped / no SoA activity.
    std::uint64_t ir_soa_generation = 0;

    constexpr LayoutStamp() noexcept = default;
    constexpr LayoutStamp(std::uint64_t aid, std::uint64_t agen, std::uint16_t fgen,
                          std::uint64_t mepoch, std::uint64_t egen, std::uint64_t dver,
                          std::uint64_t sver = 0, std::uint64_t ir_gen = 0) noexcept
        : arena_id(aid)
        , arena_gen(agen)
        , flat_gen(fgen)
        , mutation_epoch(mepoch)
        , env_gen(egen)
        , defuse_version(dver)
        , shape_version(sver)
        , ir_soa_generation(ir_gen) {}

    // operator== — full 6-field equality. A captured stamp matches
    // the current state only if EVERY field matches (per #2170
    // "consistent stamp" requirement; partial match is treated as
    // stale because Agent / FFI decisions require the whole picture
    // to be coherent).
    [[nodiscard]] constexpr bool operator==(const LayoutStamp& o) const noexcept {
        return arena_id == o.arena_id && arena_gen == o.arena_gen && flat_gen == o.flat_gen &&
               mutation_epoch == o.mutation_epoch && env_gen == o.env_gen &&
               defuse_version == o.defuse_version;
    }
    [[nodiscard]] constexpr bool operator!=(const LayoutStamp& o) const noexcept {
        return !(*this == o);
    }

    // is_any_field_zero — true if any captured field is 0 (the
    // sentinel used by lifetime_pin.hh / workspace_epoch.hh for
    // "unset / legacy ref"). Useful for tests that want to assert
    // the stamp was fully captured (no zeroes from missing fields).
    [[nodiscard]] constexpr bool is_any_field_zero() const noexcept {
        return arena_id == 0 || arena_gen == 0 || flat_gen == 0 || mutation_epoch == 0 ||
               env_gen == 0 || defuse_version == 0;
    }

    // Note: capture() helper intentionally not inlined here — it
    // needs current_mutation_epoch() from workspace_epoch.hh, and
    // pulling workspace_epoch.hh into this header would cause ODR
    // redefinition errors in TUs that also include workspace_epoch.hh
    // directly (see layout_stamp.hh preamble comment). The capture
    // logic lives in evaluator_mutation_boundary.cpp::current_layout_stamp().
};

// Schema marker — surface for the (query:stable-ref-stats-hash)
// extension's layout-stamp-schema key. Bumped when the LayoutStamp
// shape gains / loses / reorders fields. Agents use this to
// detect drift in the dashboard shape.
// Issue #2432: schema bumped for ir_soa_generation 8th field.
inline constexpr std::uint64_t kLayoutStampSchema = 2432;

} // namespace aura::core

#endif // AURA_CORE_LAYOUT_STAMP_HH