// Observability schema case table — single source of truth for domain suite.
// New Close #N stats surfaces: add a row here instead of tests/test_issue_N.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace aura::test::obs {

struct StandardCase {
    int schema;
    const char* query;
    const char* bump_slug;
};

struct FieldListCase {
    int schema;
    const char* query;
    const char* const* fields;
    std::size_t n_fields;
};

// clang-format off
inline constexpr StandardCase kStandardCases[] = {
    {830, "query:pass-shape-epoch-stats", "pass_shape_epoch"},
    {831, "query:edsl-hotpath-real-stats", "edsl_hotpath_real"},
    {832, "query:dead-coercion-elim-stats", "dead_coercion_elim"},
    {833, "query:occurrence-renarrow-stats", "occurrence_renarrow"},
    {834, "query:linear-escape-mutate-stats", "linear_escape_mutate"},
    {835, "query:typed-mutate-coercion-stats", "typed_mutate_coercion"},
    {836, "query:fiber-epoch-type-safety-stats", "fiber_epoch_type"},
    {837, "query:sv-verification-feedback-mutate-stats", "sv_feedback_mutate"},
    {838, "query:seva-longrunning-harness-v2-stats", "seva_harness_v2"},
    // Lineage #839 / #1614 / #1894 — runtime schema id is now 1894.
    {1894, "query:typed-mutation-audit-stats", "typed_mut_audit"},
    {840, "query:stable-ref-full-provenance-v2-stats", "stable_ref_full_v2"},
    {842, "query:longrunning-ai-infra-stats", "longrun_ai_infra"},
    {843, "query:ai-native-meta-extension-stats", "ai_native_meta"},
    {844, "query:orchestration-telemetry-pipeline-stats", "orch_telemetry"},
    {845, "query:per-fiber-exception-state-stats", "per_fiber_ex_state"},
    {846, "query:aot-hotswap-pipeline-stats", "aot_hotswap_pipe"},
    {847, "query:macro-hygiene-query-provenance-v2-stats", "macro_hyg_query_v2"},
    {848, "query:reflection-edsl-extension-v2-stats", "reflect_edsl_v2"},
    {849, "query:self-evolution-hygiene-dirty-epoch-stats", "selfevo_hyg_dirty"},
    {850, "query:sv-verification-feedback-closedloop-stats", "sv_fb_closedloop"},
    {851, "query:pattern-defuse-hygiene-full-stats", "pattern_defuse_hyg"},
    {852, "query:stable-ref-mutation-log-hardening-stats", "stable_ref_mutlog"},
    {853, "query:dirtyaware-impact-enforcement-v2-stats", "dirty_impact_v2"},
    {854, "query:live-irclosure-envframe-gc-stats", "live_irclosure_gc"},
    {855, "query:source-marker-linear-consistency-stats", "src_marker_linear"},
    {856, "query:terminal-buffer-diff-present-stats", "term_buf_diff"},
    {857, "query:render-observability-v2-stats", "render_obs_v2"},
    {858, "query:render-jit-soa-hotpath-stats", "render_jit_soa"},
    {859, "query:arena-live-defrag-full-v2-stats", "arena_ldefrag_v2"},
    {860, "query:ir-soa-dirty-hybrid-full-v2-stats", "irsoa_dirty_v2"},
    {861, "query:value-shape-consteval-full-v2-stats", "val_shape_ceval_v2"},
    {862, "query:defuse-infer-partial-stats", "defuse_infer_part"},
    {863, "query:ownership-escape-postmutate-stats", "own_escape_post"},
    {864, "query:typed-mutation-audit-pass-stats", "typed_audit_pass"},
    {865, "query:sv-backend-emit-bidirectional-stats", "sv_backend_bi"},
    {866, "query:large-sv-pattern-defuse-stats", "large_sv_pattern"},
    {867, "query:longrunning-stable-ref-dirty-stats", "longrun_sref_dirty"},
    {868, "query:sv-eda-primitives-cluster-stats", "sv_eda_prims"},
    {869, "query:primitives-resource-quota-fiber-stats", "prim_quota_fiber"},
    {870, "query:declarative-primitive-registry-stats", "decl_prim_reg"},
    {872, "query:primitives-namespace-alias-stats", "prim_ns_alias"},
    {875, "query:guard-steal-gc-safety-v2-stats", "guard_steal_gc_v2"},
    {876, "query:dirtyaware-ir-cache-consistency-stats", "dirty_ircache_cons"},
    {877, "query:stats-builder-refactor-stats", "stats_builder_ref"},
    {878, "query:load-or-zero-helper-stats", "load_or_zero_help"},
    {879, "query:cpp26-modernization-sweep-stats", "cpp26_mod_sweep"},
    {880, "query:metrics-meta-reflection-stats", "metrics_meta_refl"},
    {881, "query:test-harness-bootstrap-stats", "test_harness_boot"},
    {882, "query:bundle-codegen-decouple-stats", "bundle_codegen_dec"},
    {883, "query:test-bundle-migration-stats", "test_bundle_mig"},
    {884, "query:test-profile-flag-stats", "test_profile_flag"},
    {885, "query:test-harness-module-stats", "test_harness_mod"},
    {886, "query:test-json-report-stats", "test_json_report"},
    {395, "query:gcc16-modules-buildenv-stats", "gcc16_modules_env"},
};
inline constexpr std::size_t kStandardCasesCount = sizeof(kStandardCases) / sizeof(kStandardCases[0]);

inline constexpr const char* kFields_805[] = {"schema", "fastpath-hit-rate-pct", "ns-per-apply", "linear-cost", "extension-reg-ns", "bench-runs"};
inline constexpr const char* kFields_809[] = {"schema", "interop-conversions", "policy-doc-active", "hot-path-uses-result"};
inline constexpr const char* kFields_810[] = {"schema", "fiber-init-ok", "scheduler-init-ok", "aura-result-init-active"};
inline constexpr const char* kFields_811[] = {"schema", "guest-exception-bridge", "guest-only-policy-active"};
inline constexpr const char* kFields_812[] = {"schema", "steal-safety-active"};
inline constexpr const char* kFields_813[] = {"schema", "no-unwind-through-guard"};
inline constexpr const char* kFields_814[] = {"schema", "health-score"};
inline constexpr const char* kFields_815[] = {"schema", "marker-propagation-active"};
inline constexpr const char* kFields_816[] = {"schema", "define-struct-total"};
inline constexpr const char* kFields_817[] = {"schema", "marker-aware-dirty-active"};
inline constexpr const char* kFields_819[] = {"schema", "enforcement-active"};
inline constexpr const char* kFields_820[] = {"schema", "e2e-active"};
inline constexpr const char* kFields_821[] = {"schema", "fiber-local-policy-active"};
inline constexpr const char* kFields_822[] = {"schema", "l2-maturity-active"};
inline constexpr const char* kFields_823[] = {"schema", "zero-fallback-policy"};
inline constexpr const char* kFields_824[] = {"schema", "module-active"};
inline constexpr const char* kFields_825[] = {"schema", "buffer-path-active"};
inline constexpr const char* kFields_826[] = {"schema", "hotpath-active"};
inline constexpr const char* kFields_827[] = {"schema", "contracts-active"};
inline constexpr const char* kFields_828[] = {"schema", "enforcement-active"};
inline constexpr const char* kFields_829[] = {"schema", "live-defrag-active"};

inline constexpr const char* kFields_w1_955[] = {"schema", "active", "session-unregister-wired", "http-async-unified", "defuse-version-prod-api", "eval-on-current-guard", "ir-cache-max-size", "autofix-unbound-safe", "gcsweep-shared-layout", "lexer-nul-escape"};
inline constexpr const char* kFields_w1_923[] = {"schema", "active", "issue-940", "registry-domain-peels", "self-evo-safety", "list-iterative-sorts"};

inline constexpr FieldListCase kFieldListCases[] = {
    {955, "query:bugfix-941-967-stats", kFields_w1_955, sizeof(kFields_w1_955) / sizeof(kFields_w1_955[0])}, // Wave1 fold
    {923, "query:stdlib-production-review-stats", kFields_w1_923, sizeof(kFields_w1_923) / sizeof(kFields_w1_923[0])}, // Wave1 fold

    {805, "query:primitives-hotpath-registry-stats", kFields_805, sizeof(kFields_805) / sizeof(kFields_805[0])},
    {809, "query:error-handling-policy-stats", kFields_809, sizeof(kFields_809) / sizeof(kFields_809[0])},
    {810, "query:fiber-scheduler-init-stats", kFields_810, sizeof(kFields_810) / sizeof(kFields_810[0])},
    {811, "query:jit-exception-bridge-stats", kFields_811, sizeof(kFields_811) / sizeof(kFields_811[0])},
    {812, "query:orchestration-steal-arena-gc-stats", kFields_812, sizeof(kFields_812) / sizeof(kFields_812[0])},
    {813, "query:guard-error-stats", kFields_813, sizeof(kFields_813) / sizeof(kFields_813[0])},
    {814, "query:runtime-production-health", kFields_814, sizeof(kFields_814) / sizeof(kFields_814[0])},
    {815, "query:macro-introduced-provenance-stats", kFields_815, sizeof(kFields_815) / sizeof(kFields_815[0])},
    {816, "query:edsl-struct-meta-stats", kFields_816, sizeof(kFields_816) / sizeof(kFields_816[0])},
    {817, "query:dirty-epoch-marker-stats", kFields_817, sizeof(kFields_817) / sizeof(kFields_817[0])},
    {819, "query:pattern-hygiene-provenance-stats", kFields_819, sizeof(kFields_819) / sizeof(kFields_819[0])},
    {820, "query:mutate-atomic-batch-e2e-stats", kFields_820, sizeof(kFields_820) / sizeof(kFields_820[0])},
    {821, "query:jit-fiber-exception-stats", kFields_821, sizeof(kFields_821) / sizeof(kFields_821[0])},
    {822, "query:l2-specialization-deopt-stats", kFields_822, sizeof(kFields_822) / sizeof(kFields_822[0])},
    {823, "query:opcode-coverage-deopt-stats", kFields_823, sizeof(kFields_823) / sizeof(kFields_823[0])},
    {824, "query:terminal-render-production-stats", kFields_824, sizeof(kFields_824) / sizeof(kFields_824[0])},
    {825, "query:render-ffi-buffer-stats", kFields_825, sizeof(kFields_825) / sizeof(kFields_825[0])},
    {826, "query:render-hotpath-stats", kFields_826, sizeof(kFields_826) / sizeof(kFields_826[0])},
    {827, "query:shape-value-hotpath-contracts-stats", kFields_827, sizeof(kFields_827) / sizeof(kFields_827[0])},
    {828, "query:ir-soa-full-enforcement-stats", kFields_828, sizeof(kFields_828) / sizeof(kFields_828[0])},
    {829, "query:arena-live-defrag-stats", kFields_829, sizeof(kFields_829) / sizeof(kFields_829[0])},
};
inline constexpr std::size_t kFieldListCasesCount = sizeof(kFieldListCases) / sizeof(kFieldListCases[0]);
// clang-format on

} // namespace aura::test::obs
