// Production sweep / hardening / safety field-list cases — Wave 2 consolidation.
// Schema + required fields; behavioral extras stay in EXCLUDE_FROM_ALL root binaries.
// Generated/maintained for tests/domain/test_domain_production_sweep.cpp
#pragma once
#include <cstddef>
#include <cstdint>

namespace aura::test::prod {

struct ProdCase {
    int schema;
    const char* query;
    const char* const* fields;
    std::size_t n_fields;
    const char* source_stem; // original root binary (EXCLUDE_FROM_ALL)
};

// clang-format off
inline constexpr const char* kProdFields_1072[] = {"active", "http-shell-injection-fixed", "recovery-pct-clamped", "compaction-efficiency-clamped", "ast-ref-get-meta-tags", "mutate-string-bounds-bulk", "jit-fallback-status-defined", "issue-1096"};
inline constexpr const char* kProdFields_985[] = {"active", "specjit-null-placeholder-fixed", "thread-local-pressure-sample", "eda-strcat-helper", "feedback-metric-order-fixed", "set-marker-dead-ok-removed", "bounded-lru-active", "issue-1013", "quota-checks", "quota-rejects"};
inline constexpr const char* kProdFields_1047[] = {"active", "hw-coercion-empty-str-fixed", "mutation-history-void-fixed", "query-where-dedup-fixed", "eval-string-bounds-fixed", "issue-1071"};
inline constexpr const char* kProdFields_1097[] = {"active", "eval-async-heap-result", "const-fold-bool-tag-fixed", "const-fold-block-clear", "reflect-bounds-checks", "cache-header-validate-ext", "open-cache-ir-bounds", "schema-unknown-is-object", "issue-1122"};
inline constexpr const char* kProdFields_1014[] = {"active", "rebind-validation-honest", "sandbox-capability-gated", "dirty-subtree-bfs-fixed", "ir-marker-stats-hash", "defuse-string-bounds", "ir-cache-lru-active", "ir-cache-max", "issue-1046"};
inline constexpr const char* kProdFields_1123[] = {"active", "equal-zero-nil-fixed", "format-void-on-error", "term-metric-double-count-fixed", "issue-1140"};
inline constexpr const char* kProdFields_1144[] = {"active", "flat-hash-insert-helper", "selfevo-hyg-dirty-wired", "per-fiber-ex-state-wired", "orch-telemetry-wired", "dead-bump-audit-script", "issue-1148"};
inline constexpr const char* kProdFields_1158[] = {"active", "math-int64-ub-fixed", "http-get-no-shell", "git-stage-no-shell", "issue-1176", "file-path-deny-list", "renderer-module-scaffold"};
inline constexpr const char* kProdFields_1177[] = {"active", "ffi-hot-path-scaffold", "zero-copy-framebuffer-supported", "security-core-modules-scaffold", "ansi-helper-supported", "instruction-dirty-short-circuit", "fiber-join-structured", "optimization-passes-registry", "issue-1201"};
inline constexpr const char* kProdFields_1202[] = {"active", "parallel-orch-scaffold", "self-healing-hooks-active", "pure-analysis-pass-asserts", "agent-fiber-safepoint-wired", "dirty-propagation-module", "multi-fiber-mailbox-typed", "hot-path-primitives-module", "eda-parse-common-dedup", "issue-1228"};
inline constexpr const char* kProdFields_1229[] = {"active", "agent-capability-gates", "synthesize-json-escape-fixed", "ffi-opaque-tracking-hardened", "value-tag-consteval-contracts", "issue-1240"};
inline constexpr const char* kProdFields_1241[] = {"active", "soa-view-concept-enforced", "arena-shrink-tier-hardened", "soa-view-eval-helpers", "hygiene-ir-marker-propagation", "macro-clone-concurrent-hygiene", "issue-1245"};
inline constexpr const char* kProdFields_1246[] = {"active", "runtime-reflect-bridge-guard", "agent-string-heap-bounds-hardened", "stable-ref-full-path-enforced", "issue-1250"};
inline constexpr const char* kProdFields_1251[] = {"active", "mark-dirty-bounds-enforced", "rollback-compaction-path", "steal-inner-boundary-hardened", "pattern-hygiene-strict-enforced", "issue-1255", "mutation-boundary-primitives-wrapped", "mutation-hold-samples"};
inline constexpr const char* kProdFields_1256[] = {"active", "gc-safepoint-mutation-metrics", "ir-soa-cache-consistency-enforced", "panic-checkpoint-steal-hardened", "issue-1260", "mutate-guard-enforced", "ir-soa-reset-epoch-bumps"};
inline constexpr const char* kProdFields_1625[] = {"active", "aot-region-filter-enforced", "hot-update-epoch-fences", "issue-1265"};
inline constexpr const char* kProdFields_1266[] = {"active", "envframe-dualpath-enforced", "steal-starvation-mitigation", "issue-1270", "set-body-define-value-extracted"};
inline constexpr const char* kProdFields_1271[] = {"active", "runtime-obs-export-ready", "ir-hygiene-macro-marker-enforced", "hygiene-edsl-awareness", "issue-1275", "dirty-propagation-to-ir"};
inline constexpr const char* kProdFields_1276[] = {"active", "reflect-nested-struct-scaffold", "hygiene-violation-stats-active", "inline-diamond-cfg-fixed", "stable-ref-auto-refresh-enforced", "pattern-hygiene-end-to-end", "issue-1280"};
inline constexpr const char* kProdFields_1281[] = {"active", "children-topology-rollback-fidelity", "generation-wrap-restamp-policy", "provenance-boundary-hooks-active", "tree-walker-fallback-reduction", "jit-exception-opcodes-covered", "issue-1285", "provenance-boundary-capture-count", "tree-walker-define-cache-hits"};
inline constexpr const char* kProdFields_1286[] = {"active", "invalidate-per-block-dirty-active", "closure-bridge-epoch-safety-active", "guard-shape-linear-unified-active", "jit-unhandled-fail-fast-active", "ownership-lambda-params-fixed", "issue-1290", "invalidate-per-block-dirty-total"};
inline constexpr const char* kProdFields_1291[] = {"active", "fiber-spawn-fid-holder-fixed", "workspace-delete-pointer-refresh", "capability-compile-gates-active", "capability-retrofit-scaffold-active", "capability-exception-control-active", "issue-1295"};
inline constexpr const char* kProdFields_1296[] = {"active", "custom-predicate-registry-mutex", "inline-max-slot-includes-params", "ghost-orphan-free-on-rollback", "issue-1300"};
inline constexpr const char* kProdFields_1301[] = {"active", "mutation-log-compact-on-rollback", "jit-arena-env-bounds-check", "jit-closure-name-fallback-fixed", "jit-fns-overflow-map-active", "jit-closure-cache-write-lock", "issue-1305"};
inline constexpr const char* kProdFields_1306[] = {"active", "jit-string-pool-mutex", "jit-float-pool-mutex", "jit-last-module-aot-lock", "jit-closure-is-arena-flag", "jit-arena-env-free-on-reset", "issue-1310"};
inline constexpr const char* kProdFields_1311[] = {"active", "cow-boundary-pins-mutex", "jit-runtime-setters-locked", "issue-1315", "terminal-buffer-creates", "render-hotpath-samples", "render-frame-reset-total"};
inline constexpr const char* kProdFields_1321[] = {
    "active", "hotpath-contracts-expanded", "soa-view-bounds-contracts",
    "flatast-column-contracts"};

inline constexpr ProdCase kProdCases[] = {
    {1072, "query:production-hardening-1072-1096-stats", kProdFields_1072, sizeof(kProdFields_1072) / sizeof(kProdFields_1072[0]), "test_production_hardening_1072_1096"},
    {985, "query:production-hardening-985-1013-stats", kProdFields_985, sizeof(kProdFields_985) / sizeof(kProdFields_985[0]), "test_production_hardening_985_1013"},
    {1047, "query:production-safety-1047-1071-stats", kProdFields_1047, sizeof(kProdFields_1047) / sizeof(kProdFields_1047[0]), "test_production_safety_1047_1071"},
    {1097, "query:production-safety-1097-1122-stats", kProdFields_1097, sizeof(kProdFields_1097) / sizeof(kProdFields_1097[0]), "test_production_safety_1097_1122"},
    {1014, "query:production-stability-1014-1046-stats", kProdFields_1014, sizeof(kProdFields_1014) / sizeof(kProdFields_1014[0]), "test_production_stability_1014_1046"},
    {1123, "query:production-sweep-1123-1140-stats", kProdFields_1123, sizeof(kProdFields_1123) / sizeof(kProdFields_1123[0]), "test_production_sweep_1123_1140"},
    {1144, "query:production-sweep-1144-1148-stats", kProdFields_1144, sizeof(kProdFields_1144) / sizeof(kProdFields_1144[0]), "test_production_sweep_1144_1148"},
    {1158, "query:production-sweep-1158-1176-stats", kProdFields_1158, sizeof(kProdFields_1158) / sizeof(kProdFields_1158[0]), "test_production_sweep_1158_1176"},
    {1177, "query:production-sweep-1177-1201-stats", kProdFields_1177, sizeof(kProdFields_1177) / sizeof(kProdFields_1177[0]), "test_production_sweep_1177_1201"},
    {1202, "query:production-sweep-1202-1228-stats", kProdFields_1202, sizeof(kProdFields_1202) / sizeof(kProdFields_1202[0]), "test_production_sweep_1202_1228"},
    {1229, "query:production-sweep-1229-1240-stats", kProdFields_1229, sizeof(kProdFields_1229) / sizeof(kProdFields_1229[0]), "test_production_sweep_1229_1240"},
    {1241, "query:production-sweep-1241-1245-stats", kProdFields_1241, sizeof(kProdFields_1241) / sizeof(kProdFields_1241[0]), "test_production_sweep_1241_1245"},
    {1246, "query:production-sweep-1246-1250-stats", kProdFields_1246, sizeof(kProdFields_1246) / sizeof(kProdFields_1246[0]), "test_production_sweep_1246_1250"},
    {1251, "query:production-sweep-1251-1255-stats", kProdFields_1251, sizeof(kProdFields_1251) / sizeof(kProdFields_1251[0]), "test_production_sweep_1251_1255"},
    {1256, "query:production-sweep-1256-1260-stats", kProdFields_1256, sizeof(kProdFields_1256) / sizeof(kProdFields_1256[0]), "test_production_sweep_1256_1260"},
    {1625, "query:production-sweep-1261-1265-stats", kProdFields_1625, sizeof(kProdFields_1625) / sizeof(kProdFields_1625[0]), "test_production_sweep_1261_1265"},
    {1266, "query:production-sweep-1266-1270-stats", kProdFields_1266, sizeof(kProdFields_1266) / sizeof(kProdFields_1266[0]), "test_production_sweep_1266_1270"},
    {1271, "query:production-sweep-1271-1275-stats", kProdFields_1271, sizeof(kProdFields_1271) / sizeof(kProdFields_1271[0]), "test_production_sweep_1271_1275"},
    {1276, "query:production-sweep-1276-1280-stats", kProdFields_1276, sizeof(kProdFields_1276) / sizeof(kProdFields_1276[0]), "test_production_sweep_1276_1280"},
    {1281, "query:production-sweep-1281-1285-stats", kProdFields_1281, sizeof(kProdFields_1281) / sizeof(kProdFields_1281[0]), "test_production_sweep_1281_1285"},
    {1286, "query:production-sweep-1286-1290-stats", kProdFields_1286, sizeof(kProdFields_1286) / sizeof(kProdFields_1286[0]), "test_production_sweep_1286_1290"},
    {1291, "query:production-sweep-1291-1295-stats", kProdFields_1291, sizeof(kProdFields_1291) / sizeof(kProdFields_1291[0]), "test_production_sweep_1291_1295"},
    {1296, "query:production-sweep-1296-1300-stats", kProdFields_1296, sizeof(kProdFields_1296) / sizeof(kProdFields_1296[0]), "test_production_sweep_1296_1300"},
    {1301, "query:production-sweep-1301-1305-stats", kProdFields_1301, sizeof(kProdFields_1301) / sizeof(kProdFields_1301[0]), "test_production_sweep_1301_1305"},
    {1306, "query:production-sweep-1306-1310-stats", kProdFields_1306, sizeof(kProdFields_1306) / sizeof(kProdFields_1306[0]), "test_production_sweep_1306_1310"},
    {1311, "query:production-sweep-1311-1315-stats", kProdFields_1311, sizeof(kProdFields_1311) / sizeof(kProdFields_1311[0]), "test_production_sweep_1311_1315"},
    {1321, "query:production-sweep-1321-1324-stats", kProdFields_1321, sizeof(kProdFields_1321) / sizeof(kProdFields_1321[0]), "test_production_sweep_1321_1324"},
};
inline constexpr std::size_t kProdCasesCount = sizeof(kProdCases) / sizeof(kProdCases[0]);
// clang-format on

} // namespace aura::test::prod

