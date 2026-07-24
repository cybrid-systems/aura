// tests/compiler/obs_schema_cases.hpp — observability schema case table.
//
// History:
//   - Originally expected at tests/domain/cases/obs_schema_cases.hpp but the
//     file never landed (only references in docs/scripts/legacy inventory).
//   - During the tests/ reorg (R1 7fd24ede → R11 b5a30303), this header was
//     created as a placeholder in tests/compiler/obs_schema_cases.hpp.
//   - R12 ships the populating data: StandardCase + FieldListCase + arrays.
//
// Schema field convention: schema = issue# that introduced the surface
// (mirrors tests/compiler/production_sweep_cases.hpp pattern: 1072/985/1047/1097).
// For sentinel-surfaces src/ comments hardcode schema == N (e.g. schema==642
// for query:arena-auto-compaction-stats, schema==543 for envframe-dualpath).
//
// Update discipline:
//   - When adding a new (query:*-stats) bump surface, add a row to
//     kStandardCases with the issue#.
//   - When adding a new (query:field-list-stats), add a row to kFieldListCases.
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
inline constexpr const char* kFields_envframe_dualpath[] = {
    "stale-count", "bump-count", "schema"};
inline constexpr const char* kFields_pattern_index[] = {
    "active", "rebuilt", "schema"};
inline constexpr const char* kFields_macro_hygiene[] = {
    "active", "macro-introduced-count", "cloned-count", "schema"};
inline constexpr const char* kFields_typed_mutation[] = {
    "active", "mutations", "violations", "schema"};
inline constexpr const char* kFields_reflect_edsl[] = {
    "active", "bridges", "hygiene-rejects", "schema"};
inline constexpr const char* kFields_selfevolution[] = {
    "active", "stable", "rollback-count", "schema"};
inline constexpr const char* kFields_arena_concurrent[] = {
    "active", "compact-total", "defrag-total", "schema"};
inline constexpr const char* kFields_occurrence_narrow[] = {
    "active", "renarrow-total", "renarrow-savings", "schema"};
inline constexpr const char* kFields_arena_compaction_sweep[] = {
    "active", "compaction-efficiency", "live-roots", "schema"};
inline constexpr const char* kFields_jit_soa[] = {
    "active", "soa-views", "soa-eval-savings", "schema"};
inline constexpr const char* kFields_dirty_propagation[] = {
    "active", "cascade-depth", "dirty-mark-count", "schema"};

// Standard cases — schema + query + bump_slug. Order matches bump_standard switch
// in tests/compiler/test_obs_schema_matrix.cpp:62-…
inline constexpr std::array<StandardCase, 47> kStandardCases{{
    // 1: shape-jit-pass-closedloop stats (#712)
    {712, "query:shape-jit-pass-closedloop-stats", "pass_shape_epoch"},
    // 2: edsl-core-stability stats (#713)
    {713, "query:edsl-core-stability-stats", "edsl_hotpath_real"},
    // 3: type-incremental stats (#714)
    {714, "query:type-incremental-stats", "dead_coercion_elim"},
    // 4: occurrence-narrow stats (#715)
    {715, "query:occurrence-narrow-stats", "occurrence_renarrow"},
    // 5: linear-boundary-consistency stats (#716)
    {716, "query:linear-boundary-consistency-stats", "linear_escape_mutate"},
    // 6: typed-mutation stats (#717)
    {717, "query:typed-mutation-stats", "typed_mutate_coercion"},
    // 7: fiber-epoch-type stats (#718)
    {718, "query:fiber-epoch-type-stats", "fiber_epoch_type"},
    // 8: sv-feedback-mutate stats (#719)
    {719, "query:sv-feedback-mutate-stats", "sv_feedback_mutate"},
    // 9: seva-longrunning-concurrent-slo (#720)
    {720, "query:seva-longrunning-concurrent-slo", "seva_harness_v2"},
    // 10: typed-mutation-audit-trail (#721)
    {721, "query:typed-mutation-audit-trail", "typed_mut_audit"},
    // 11: stable-ref-full-v2 stats (#728)
    {728, "query:stable-ref-full-v2-stats", "stable_ref_full_v2"},
    // 12: longrunning-infra stats (#751)
    {751, "query:longrunning-infra-stats", "longrun_ai_infra"},
    // 13: ai-native-meta-extension stats (#752)
    {752, "query:ai-native-meta-extension-stats", "ai_native_meta"},
    // 14: orch-telemetry stats (#753)
    {753, "query:orch-telemetry-stats", "orch_telemetry"},
    // 15: per-fiber-ex-state stats (#754)
    {754, "query:per-fiber-ex-state-stats", "per_fiber_ex_state"},
    // 16: aot-hotswap-pipeline stats (#755)
    {755, "query:aot-hotswap-pipeline-stats", "aot_hotswap_pipe"},
    // 17: macro-hygiene stats (#486, pre-pipeline)
    {486, "query:macro-hygiene-stats", "macro_hyg_query_v2"},
    // 18: reflect-edsl-bridge stats (#141 — Issue #1907)
    {141, "query:reflect-edsl-bridge-stats", "reflect_edsl_v2"},
    // 19: self-evolution-stability stats (#549)
    {549, "query:self-evolution-stability-stats", "selfevo_hyg_dirty"},
    // 20: sv-fb-closedloop stats (#733)
    {733, "query:sv-fb-closedloop-stats", "sv_fb_closedloop"},
    // 21: pattern-defuse-hygiene stats (#734)
    {734, "query:pattern-defuse-hyg-stats", "pattern_defuse_hyg"},
    // 22: stable-ref-mutlog stats (#735)
    {735, "query:stable-ref-mutlog-stats", "stable_ref_mutlog"},
    // 23: dirty-impact-v2 stats (#736)
    {736, "query:dirty-impact-v2", "dirty_impact_v2"},
    // 24: live-irclosure-gc stats (#737)
    {737, "query:live-irclosure-gc-stats", "live_irclosure_gc"},
    // 25: src-marker-linear stats (#738)
    {738, "query:src-marker-linear-stats", "src_marker_linear"},
    // 26: term-buf-diff stats (#739)
    {739, "query:term-buf-diff-stats", "term_buf_diff"},
    // 27: render-obs-v2 stats (#741)
    {741, "query:render-obs-v2-stats", "render_obs_v2"},
    // 28: render-jit-soa stats (#742)
    {742, "query:render-jit-soa-stats", "render_jit_soa"},
    // 29: arena-auto-compact stats (Issue #1989 / #1988 series)
    {1989, "query:arena-auto-compact-stats", "arena_ldefrag_v2"},
    // 30: irsoa-dirty stats (#743)
    {743, "query:irsoa-dirty-stats", "irsoa_dirty_v2"},
    // 31: val-shape-ceval stats (#744)
    {744, "query:val-shape-ceval-stats", "val_shape_ceval_v2"},
    // 32: defuse-infer-part stats (#745)
    {745, "query:defuse-infer-part-stats", "defuse_infer_part"},
    // 33: own-escape-post stats (#746)
    {746, "query:own-escape-post-stats", "own_escape_post"},
    // 34: typed-audit-pass stats (#747)
    {747, "query:typed-audit-pass-stats", "typed_audit_pass"},
    // 35: sv-backend-bi stats (#748)
    {748, "query:sv-backend-bi-stats", "sv_backend_bi"},
    // 36: large-sv-pattern stats (#749)
    {749, "query:large-sv-pattern-stats", "large_sv_pattern"},
    // 37: longrun-sref-dirty stats (#750)
    {750, "query:longrun-sref-dirty-stats", "longrun_sref_dirty"},
    // 38: sv-eda-prims stats (#756)
    {756, "query:sv-eda-prims-stats", "sv_eda_prims"},
    // 39: prim-quota-fiber stats (#757)
    {757, "query:prim-quota-fiber-stats", "prim_quota_fiber"},
    // 40: decl-prim-reg stats (#758)
    {758, "query:decl-prim-reg-stats", "decl_prim_reg"},
    // 41: prim-ns-alias stats (#759)
    {759, "query:prim-ns-alias-stats", "prim_ns_alias"},
    // 42: guard-steal-gc stats (#760)
    {760, "query:guard-steal-gc-stats", "guard_steal_gc_v2"},
    // 43: dirty-ircache-cons stats (#761)
    {761, "query:dirty-ircache-cons-stats", "dirty_ircache_cons"},
    // 44: stats-builder-ref stats (#762)
    {762, "query:stats-builder-ref-stats", "stats_builder_ref"},
    // 45: load-or-zero-help stats (#763)
    {763, "query:load-or-zero-help-stats", "load_or_zero_help"},
    // 46: cpp26-mod-sweep stats (#764)
    {764, "query:cpp26-mod-sweep-stats", "cpp26_mod_sweep"},
    // 47: metrics-meta-refl stats (#765)
    {765, "query:metrics-meta-refl-stats", "metrics_meta_refl"},
}};

// Field list cases — schema + query + fields[] + n_fields
inline constexpr std::array<FieldListCase, 12> kFieldListCases{{
    // Issue #642 sentinel — arena-auto-compaction-stats field-list (drift detection)
    {642, "query:arena-auto-compaction-stats", kFields_arena_auto_compact,
     sizeof(kFields_arena_auto_compact) / sizeof(kFields_arena_auto_compact[0])},
    // Issue #543 — envframe-dualpath-stats
    {543, "query:envframe-dualpath-stats", kFields_envframe_dualpath,
     sizeof(kFields_envframe_dualpath) / sizeof(kFields_envframe_dualpath[0])},
    // Issue #547 — pattern-index-stats
    {547, "query:pattern-index-stats", kFields_pattern_index,
     sizeof(kFields_pattern_index) / sizeof(kFields_pattern_index[0])},
    // Issue #486 — macro-hygiene-stats field-list
    {486, "query:macro-hygiene-stats", kFields_macro_hygiene,
     sizeof(kFields_macro_hygiene) / sizeof(kFields_macro_hygiene[0])},
    // Issue #550 — typed-mutation-stats field-list
    {550, "query:typed-mutation-stats", kFields_typed_mutation,
     sizeof(kFields_typed_mutation) / sizeof(kFields_typed_mutation[0])},
    // Issue #1907 — reflect-edsl-bridge-stats field-list
    {1907, "query:reflect-edsl-bridge-stats", kFields_reflect_edsl,
     sizeof(kFields_reflect_edsl) / sizeof(kFields_reflect_edsl[0])},
    // Issue #549 — self-evolution-stability field-list
    {549, "query:self-evolution-stability-stats", kFields_selfevolution,
     sizeof(kFields_selfevolution) / sizeof(kFields_selfevolution[0])},
    // Issue #1988 — arena-concurrent-compact field-list
    {1988, "query:arena-concurrent-compact-stats", kFields_arena_concurrent,
     sizeof(kFields_arena_concurrent) / sizeof(kFields_arena_concurrent[0])},
    // Issue #715 — occurrence-narrow field-list (subsurface)
    {715, "query:occurrence-narrow-stats", kFields_occurrence_narrow,
     sizeof(kFields_occurrence_narrow) / sizeof(kFields_occurrence_narrow[0])},
    // Issue #642 — arena-compaction-sweep field-list (Wave2 production sweep)
    {1625, "query:arena-compaction-sweep-stats", kFields_arena_compaction_sweep,
     sizeof(kFields_arena_compaction_sweep) / sizeof(kFields_arena_compaction_sweep[0])},
    // Issue #742 — render-jit-soa field-list
    {742, "query:render-jit-soa-stats", kFields_jit_soa,
     sizeof(kFields_jit_soa) / sizeof(kFields_jit_soa[0])},
    // Issue #736 — dirty-propagation field-list
    {736, "query:dirty-propagation-stats", kFields_dirty_propagation,
     sizeof(kFields_dirty_propagation) / sizeof(kFields_dirty_propagation[0])},
}};

inline constexpr std::size_t kStandardCasesCount = kStandardCases.size();
inline constexpr std::size_t kFieldListCasesCount = kFieldListCases.size();

} // namespace aura::test::obs
