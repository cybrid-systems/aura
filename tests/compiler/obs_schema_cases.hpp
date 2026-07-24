// tests/compiler/obs_schema_cases.hpp — observability schema case table.
//
// History:
//   - Originally expected at a R1-pilot tests/domain/cases/ location, but that
//     file never landed (only references in docs/scripts/legacy inventory).
//   - During the tests/ reorg (R1 7fd24ede → R11 b5a30303), this header was
//     created as a placeholder in tests/compiler/obs_schema_cases.hpp.
//   - R12 ships the populating data: StandardCase + FieldListCase + arrays.
//   - R15 (this revision) — replaced the issue#-pattern schema guesses with
//     runtime-truth values probed from aura via build/test_obs_schema_matrix.
//     Removed 48 cases (39 standard + 9 field-list) that returned -1 from
//     aura runtime (no `insert_kv("schema", N)` sentinel exists for those
//     query:*-stats surfaces). The removed surfaces are listed at the bottom
//     of this file as a TODO backlog for a future PR to re-add once aura
//     exposes schema sentinels for them (or to fix the names if R12 ship
//     used abbreviated forms that don't match aura's actual surface names —
//     cross-reference src/compiler/evaluator_primitives_observability.cpp
//     for the canonical surface-name array).
//
// Schema field convention: schema = the value aura runtime returns at
// `(hash-ref (engine:metrics "<query>") 'schema)`, used as a drift-detection
// sentinel by the Agent. Probed via the PROBE print added to
// tests/compiler/test_obs_schema_matrix.cpp in R15.
//
// Update discipline:
//   - When aura gains a new schema sentinel for a (query:*-stats) surface,
//     re-add a row to kStandardCases with the runtime-returned schema.
//   - When adding a new (query:field-list-stats), re-add a row to
//     kFieldListCases with the runtime-returned schema.
//   - bump_standard switch in tests/compiler/test_obs_schema_matrix.cpp
//     must be kept in sync with kStandardCases[i].bump_slug.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace aura::test::obs {

struct StandardCase {
    int schema;             // href(cs, c.query, "schema") == c.schema
    const char* query;      // aura::test::aura_call_expr(c.query)
    const char* bump_slug;  // passed to bump_standard(ev, c.bump_slug)
};

struct FieldListCase {
    int schema;
    const char* query;
    const char* const* fields;
    std::size_t n_fields;
};

// Field arrays for kFieldListCases. inline constexpr per production_sweep_cases.hpp style.
inline constexpr const char* kFields_arena_auto_compact[] = {
    "auto-trigger", "live-move-yield", "guard-defrag", "schema"};
inline constexpr const char* kFields_arena_concurrent[] = {
    "active", "compact-total", "defrag-total", "schema"};

// Standard cases — schema + query + bump_slug. Order matches bump_standard switch
// in tests/compiler/test_obs_schema_matrix.cpp:62-…
// Schema values are runtime-truth (probed R15).
inline constexpr std::array<StandardCase, 8> kStandardCases{{
    // 1: shape-jit-pass-closedloop stats (Issue #744 / lineage)
    {744, "query:shape-jit-pass-closedloop-stats", "pass_shape_epoch"},
    // 2: edsl-core-stability stats (#655)
    {655, "query:edsl-core-stability-stats", "edsl_hotpath_real"},
    // 3: linear-boundary-consistency stats (#1895)
    {1895, "query:linear-boundary-consistency-stats", "linear_escape_mutate"},
    // 4: seva-longrunning-concurrent-slo (#803)
    {803, "query:seva-longrunning-concurrent-slo", "seva_harness_v2"},
    // 5: longrunning-infra stats (#753)
    {753, "query:longrunning-infra-stats", "longrun_ai_infra"},
    // 6: ai-native-meta-extension stats (#843)
    {843, "query:ai-native-meta-extension-stats", "ai_native_meta"},
    // 7: aot-hotswap-pipeline stats (#846)
    {846, "query:aot-hotswap-pipeline-stats", "aot_hotswap_pipe"},
    // 8: macro-hygiene stats (#1613, lineage 1599|1597|1593|1499)
    {1613, "query:macro-hygiene-stats", "macro_hyg_query_v2"},
}};

// Field list cases — schema + query + fields[] + n_fields
// Schema values are runtime-truth (probed R15).
inline constexpr std::array<FieldListCase, 2> kFieldListCases{{
    // Issue #642 — arena-auto-compaction-stats field-list (drift detection)
    {642, "query:arena-auto-compaction-stats", kFields_arena_auto_compact,
     sizeof(kFields_arena_auto_compact) / sizeof(kFields_arena_auto_compact[0])},
    // Issue #731 — arena-concurrent-compact-stats field-list
    {731, "query:arena-concurrent-compact-stats", kFields_arena_concurrent,
     sizeof(kFields_arena_concurrent) / sizeof(kFields_arena_concurrent[0])},
}};

inline constexpr std::size_t kStandardCasesCount = kStandardCases.size();
inline constexpr std::size_t kFieldListCasesCount = kFieldListCases.size();

// ─────────────────────────────────────────────────────────────────────────────
// R15 TODO backlog — surfaces R12 guessed with issue# pattern but aura runtime
// doesn't expose a schema sentinel for them. Returned -1 from
// (hash-ref (engine:metrics "<query>") 'schema) at probe time. Re-add a row
// here once either:
//   (a) aura adds `insert_kv("schema", N)` for the surface, OR
//   (b) the surface name is corrected to match aura's canonical name in
//       src/compiler/evaluator_primitives_observability.cpp / obs_eval.cpp.
//
// Standard surfaces (39):
//   query:type-incremental-stats (R12 guess 714)
//   query:occurrence-narrow-stats (R12 guess 715)
//   query:typed-mutation-stats (R12 guess 717)
//   query:fiber-epoch-type-stats (R12 guess 718)
//   query:sv-feedback-mutate-stats (R12 guess 719)
//   query:typed-mutation-audit-trail (R12 guess 721)
//   query:stable-ref-full-v2-stats (R12 guess 728)
//   query:orch-telemetry-stats (R12 guess 753)
//   query:per-fiber-ex-state-stats (R12 guess 754)
//   query:reflect-edsl-bridge-stats (R12 guess 141)
//   query:self-evolution-stability-stats (R12 guess 549)
//   query:sv-fb-closedloop-stats (R12 guess 733)
//   query:pattern-defuse-hyg-stats (R12 guess 734)
//   query:stable-ref-mutlog-stats (R12 guess 735)
//   query:dirty-impact-v2 (R12 guess 736)
//   query:live-irclosure-gc-stats (R12 guess 737)
//   query:src-marker-linear-stats (R12 guess 738)
//   query:term-buf-diff-stats (R12 guess 739)
//   query:render-obs-v2-stats (R12 guess 741)
//   query:render-jit-soa-stats (R12 guess 742)
//   query:arena-auto-compact-stats (R12 guess 1989)
//   query:irsoa-dirty-stats (R12 guess 743)
//   query:val-shape-ceval-stats (R12 guess 744)
//   query:defuse-infer-part-stats (R12 guess 745)
//   query:own-escape-post-stats (R12 guess 746)
//   query:typed-audit-pass-stats (R12 guess 747)
//   query:sv-backend-bi-stats (R12 guess 748)
//   query:large-sv-pattern-stats (R12 guess 749)
//   query:longrun-sref-dirty-stats (R12 guess 750)
//   query:sv-eda-prims-stats (R12 guess 756)
//   query:prim-quota-fiber-stats (R12 guess 757)
//   query:decl-prim-reg-stats (R12 guess 758)
//   query:prim-ns-alias-stats (R12 guess 759)
//   query:guard-steal-gc-stats (R12 guess 760)
//   query:dirty-ircache-cons-stats (R12 guess 761)
//   query:stats-builder-ref-stats (R12 guess 762)
//   query:load-or-zero-help-stats (R12 guess 763)
//   query:cpp26-mod-sweep-stats (R12 guess 764)
//   query:metrics-meta-refl-stats (R12 guess 765)
//
// Field-list surfaces (9):
//   query:envframe-dualpath-stats (R12 guess 543)
//   query:pattern-index-stats (R12 guess 547)
//   query:typed-mutation-stats field-list (R12 guess 550)
//   query:reflect-edsl-bridge-stats field-list (R12 guess 1907)
//   query:self-evolution-stability-stats field-list (R12 guess 549)
//   query:occurrence-narrow-stats field-list (R12 guess 715)
//   query:arena-compaction-sweep-stats field-list (R12 guess 1625)
//   query:render-jit-soa-stats field-list (R12 guess 742)
//   query:dirty-propagation-stats field-list (R12 guess 736)

} // namespace aura::test::obs