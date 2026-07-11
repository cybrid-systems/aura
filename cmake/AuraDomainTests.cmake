# Domain test suites — preferred issue gates (issue id = label, not process).
#
# Sources live under tests/domain/*.cpp with shared case tables in
# tests/domain/cases/. New observability / fiber / hygiene / typed ACs
# should extend these targets instead of adding tests/test_issue_N.cpp.
#
# Included from CMakeLists.txt after all_test_issue_targets exists and
# aura_issue_test_link_llvm_jit is available.

# Legacy Phase 1 batches — superseded by domain suites.
# EXCLUDE_FROM_ALL: on-demand only (`ninja test_issues_809_817_batch`).
aura_add_issue_test(test_issues_809_817_batch)
aura_issue_test_link_llvm_jit(test_issues_809_817_batch)
set_target_properties(test_issues_809_817_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)

aura_add_issue_test(test_issues_819_829_batch)
aura_issue_test_link_llvm_jit(test_issues_819_829_batch)
set_target_properties(test_issues_819_829_batch PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Bundle member / legacy alias — prefer test_obs_schema_matrix.
aura_add_issue_test(test_open_issues_phase1_batch)
aura_issue_test_link_llvm_jit(test_open_issues_phase1_batch)

# Preferred domain suites (in all_test_issue_targets + issues_fast.json).
aura_add_issue_test(test_obs_schema_matrix)
aura_issue_test_link_llvm_jit(test_obs_schema_matrix)
add_dependencies(all_test_issue_targets test_obs_schema_matrix)

# Issues #923–#940: stdlib production review Phase 1
aura_add_issue_test(test_stdlib_production_review_923_940)
aura_issue_test_link_llvm_jit(test_stdlib_production_review_923_940)
add_dependencies(all_test_issue_targets test_stdlib_production_review_923_940)

# Issues #941–#967: self-evo pipeline + bugfix Phase 1
aura_add_issue_test(test_selfevo_bugfix_941_967)
aura_issue_test_link_llvm_jit(test_selfevo_bugfix_941_967)
add_dependencies(all_test_issue_targets test_selfevo_bugfix_941_967)

# Issues #968–#984: observability / JIT / typechecker / FFI / arena bugfixes
aura_add_issue_test(test_bugfix_968_984)
aura_issue_test_link_llvm_jit(test_bugfix_968_984)
add_dependencies(all_test_issue_targets test_bugfix_968_984)

# Issues #985–#1013: production cache bounds + resource quota Phase 1
aura_add_issue_test(test_production_hardening_985_1013)
aura_issue_test_link_llvm_jit(test_production_hardening_985_1013)
add_dependencies(all_test_issue_targets test_production_hardening_985_1013)

# Issues #1014–#1046: production stability + mutation/security bugfixes Phase 1
aura_add_issue_test(test_production_stability_1014_1046)
aura_issue_test_link_llvm_jit(test_production_stability_1014_1046)
add_dependencies(all_test_issue_targets test_production_stability_1014_1046)

# Issues #1047–#1071: hygiene / type / mutate safety Phase 1
aura_add_issue_test(test_production_safety_1047_1071)
aura_issue_test_link_llvm_jit(test_production_safety_1047_1071)
add_dependencies(all_test_issue_targets test_production_safety_1047_1071)

# Issues #1072–#1096: security / metrics / concurrency Phase 1
aura_add_issue_test(test_production_hardening_1072_1096)
aura_issue_test_link_llvm_jit(test_production_hardening_1072_1096)
add_dependencies(all_test_issue_targets test_production_hardening_1072_1096)

# Issues #1097–#1122: serialize / fold / serve safety Phase 1
aura_add_issue_test(test_production_safety_1097_1122)
aura_issue_test_link_llvm_jit(test_production_safety_1097_1122)
add_dependencies(all_test_issue_targets test_production_safety_1097_1122)

# Issues #1123–#1140: final open-issue sweep Phase 1
aura_add_issue_test(test_production_sweep_1123_1140)
aura_issue_test_link_llvm_jit(test_production_sweep_1123_1140)
add_dependencies(all_test_issue_targets test_production_sweep_1123_1140)

# Issues #1144–#1148: observability wire-up / dead-bump audit Phase 1
aura_add_issue_test(test_production_sweep_1144_1148)
aura_issue_test_link_llvm_jit(test_production_sweep_1144_1148)
add_dependencies(all_test_issue_targets test_production_sweep_1144_1148)

# Issues #1150–#1156: checked arithmetic / coerce safety Phase 1
aura_add_issue_test(test_arithmetic_int64_safety)
aura_issue_test_link_llvm_jit(test_arithmetic_int64_safety)
add_dependencies(all_test_issue_targets test_arithmetic_int64_safety)

# Issues #1158–#1176: math UB + IO security + stdlib review Phase 1
aura_add_issue_test(test_production_sweep_1158_1176)
aura_issue_test_link_llvm_jit(test_production_sweep_1158_1176)
add_dependencies(all_test_issue_targets test_production_sweep_1158_1176)

# Issues #1177–#1201: render/FFI/security/orchestration Phase 1
aura_add_issue_test(test_production_sweep_1177_1201)
aura_issue_test_link_llvm_jit(test_production_sweep_1177_1201)
add_dependencies(all_test_issue_targets test_production_sweep_1177_1201)

# Issues #1202–#1228: orchestration / heal / memory / observability Phase 1
aura_add_issue_test(test_production_sweep_1202_1228)
aura_issue_test_link_llvm_jit(test_production_sweep_1202_1228)
add_dependencies(all_test_issue_targets test_production_sweep_1202_1228)

# Issues #1229–#1240: EDA/FFI/agent security + verification Phase 1
aura_add_issue_test(test_production_sweep_1229_1240)
aura_issue_test_link_llvm_jit(test_production_sweep_1229_1240)
add_dependencies(all_test_issue_targets test_production_sweep_1229_1240)

# Issues #1241–#1245: SoAView / arena / hygiene concurrent Phase 1
aura_add_issue_test(test_production_sweep_1241_1245)
aura_issue_test_link_llvm_jit(test_production_sweep_1241_1245)
add_dependencies(all_test_issue_targets test_production_sweep_1241_1245)

# Issues #1246–#1250: reflect / hygiene / agent OOB / StableNodeRef Phase 1
aura_add_issue_test(test_production_sweep_1246_1250)
aura_issue_test_link_llvm_jit(test_production_sweep_1246_1250)
add_dependencies(all_test_issue_targets test_production_sweep_1246_1250)

# Issues #1251–#1255: dirty/Guard/steal/pattern Phase 1
aura_add_issue_test(test_production_sweep_1251_1255)
aura_issue_test_link_llvm_jit(test_production_sweep_1251_1255)
add_dependencies(all_test_issue_targets test_production_sweep_1251_1255)

# Issues #1256–#1260: GC/workspace/IR/mutate-guard/panic Phase 1
aura_add_issue_test(test_production_sweep_1256_1260)
aura_issue_test_link_llvm_jit(test_production_sweep_1256_1260)
add_dependencies(all_test_issue_targets test_production_sweep_1256_1260)

# Issues #1261–#1265: dep_graph/AOT/arena/hotswap/QAR Phase 1
aura_add_issue_test(test_production_sweep_1261_1265)
aura_issue_test_link_llvm_jit(test_production_sweep_1261_1265)
add_dependencies(all_test_issue_targets test_production_sweep_1261_1265)

# Issues #1266–#1270: inline/set-body/panic/SoA/steal Phase 1
aura_add_issue_test(test_production_sweep_1266_1270)
aura_issue_test_link_llvm_jit(test_production_sweep_1266_1270)
add_dependencies(all_test_issue_targets test_production_sweep_1266_1270)

# Issues #1271–#1275: AOT/obs/hygiene-IR/dirty/EDSL Phase 1
aura_add_issue_test(test_production_sweep_1271_1275)
aura_issue_test_link_llvm_jit(test_production_sweep_1271_1275)
add_dependencies(all_test_issue_targets test_production_sweep_1271_1275)

# Issues #1276–#1280: reflect/obs/inliner/StableRef/pattern Phase 1
aura_add_issue_test(test_production_sweep_1276_1280)
aura_issue_test_link_llvm_jit(test_production_sweep_1276_1280)
add_dependencies(all_test_issue_targets test_production_sweep_1276_1280)

# Issues #1281–#1285: children rollback/gen wrap/provenance/fallback/JIT EH Phase 1
aura_add_issue_test(test_production_sweep_1281_1285)
aura_issue_test_link_llvm_jit(test_production_sweep_1281_1285)
add_dependencies(all_test_issue_targets test_production_sweep_1281_1285)

# Issues #1286–#1290: invalidate/block-dirty, closure epoch, GuardShape, JIT fail-fast, ownership Lambda
aura_add_issue_test(test_production_sweep_1286_1290)
aura_issue_test_link_llvm_jit(test_production_sweep_1286_1290)
add_dependencies(all_test_issue_targets test_production_sweep_1286_1290)

# Issues #1291–#1295: fiber fid, workspace UAF, compile/fiber/exception caps
aura_add_issue_test(test_production_sweep_1291_1295)
aura_issue_test_link_llvm_jit(test_production_sweep_1291_1295)
add_dependencies(all_test_issue_targets test_production_sweep_1291_1295)

# Issues #1296–#1300: predicate race, inline max_slot, ghost orphan free
aura_add_issue_test(test_production_sweep_1296_1300)
aura_issue_test_link_llvm_jit(test_production_sweep_1296_1300)
add_dependencies(all_test_issue_targets test_production_sweep_1296_1300)

# Issues #1301–#1305: mutation_log compact, arena OOB, name fallback, fn overflow, cache TOCTOU
aura_add_issue_test(test_production_sweep_1301_1305)
aura_issue_test_link_llvm_jit(test_production_sweep_1301_1305)
add_dependencies(all_test_issue_targets test_production_sweep_1301_1305)

# Issues #1306–#1310: string/float pool races, last_module lock, is_arena, free envs
aura_add_issue_test(test_production_sweep_1306_1310)
aura_issue_test_link_llvm_jit(test_production_sweep_1306_1310)
add_dependencies(all_test_issue_targets test_production_sweep_1306_1310)

# Issues #1311–#1315: cow pins race, jit setters, terminal buffer, present batch, render arena
aura_add_issue_test(test_production_sweep_1311_1315)
aura_issue_test_link_llvm_jit(test_production_sweep_1311_1315)
add_dependencies(all_test_issue_targets test_production_sweep_1311_1315)

# Issues #1316–#1320: render JIT stability, render obs meta, SoA migrate, gap buffer, defrag
aura_add_issue_test(test_production_sweep_1316_1320)
aura_issue_test_link_llvm_jit(test_production_sweep_1316_1320)
add_dependencies(all_test_issue_targets test_production_sweep_1316_1320)

# Issues #1321–#1324: C++26 contracts expand, dirty pipeline, JIT map races
aura_add_issue_test(test_production_sweep_1321_1324)
aura_issue_test_link_llvm_jit(test_production_sweep_1321_1324)
add_dependencies(all_test_issue_targets test_production_sweep_1321_1324)

# Issues #1325–#1330: primitive surface reduction architecture (META + phases 1–5)
aura_add_issue_test(test_production_sweep_1325_1330)
aura_issue_test_link_llvm_jit(test_production_sweep_1325_1330)
add_dependencies(all_test_issue_targets test_production_sweep_1325_1330)

# Issues #1331–#1343: 5-layer TUI pixel rendering architecture
aura_add_issue_test(test_production_sweep_1331_1343)
aura_issue_test_link_llvm_jit(test_production_sweep_1331_1343)
add_dependencies(all_test_issue_targets test_production_sweep_1331_1343)

# Issues #1336–#1341, #1344–#1348: type/AST/EDA production sweep
aura_add_issue_test(test_production_sweep_1336_1348)
aura_issue_test_link_llvm_jit(test_production_sweep_1336_1348)
add_dependencies(all_test_issue_targets test_production_sweep_1336_1348)

# Issue #1349: terminal-present-batch ANSI SGR + CSI H (P0 cyber-cat)
aura_add_issue_test(test_terminal_ansi_emit)
aura_issue_test_link_llvm_jit(test_terminal_ansi_emit)
add_dependencies(all_test_issue_targets test_terminal_ansi_emit)

# Issue #1350: 24-bit RGB + Unicode terminal cells
aura_add_issue_test(test_terminal_rgb)
aura_issue_test_link_llvm_jit(test_terminal_rgb)
add_dependencies(all_test_issue_targets test_terminal_rgb)

# Issue #1351: deprecate 7 no-op terminal:* primitives
aura_add_issue_test(test_terminal_deprecation)
aura_issue_test_link_llvm_jit(test_terminal_deprecation)
add_dependencies(all_test_issue_targets test_terminal_deprecation)

# Issue #1352: terminal buffer lifecycle + per-buffer mutex
aura_add_issue_test(test_terminal_lifecycle)
aura_issue_test_link_llvm_jit(test_terminal_lifecycle)
add_dependencies(all_test_issue_targets test_terminal_lifecycle)
aura_add_issue_test(test_terminal_concurrent)
aura_issue_test_link_llvm_jit(test_terminal_concurrent)
add_dependencies(all_test_issue_targets test_terminal_concurrent)

# Issue #1353: keyboard raw mode + non-blocking poll
aura_add_issue_test(test_terminal_input)
aura_issue_test_link_llvm_jit(test_terminal_input)
add_dependencies(all_test_issue_targets test_terminal_input)

# Issue #1354: render FFI hot path + c-render-bind discovery
aura_add_issue_test(test_render_ffi_hotpath)
aura_issue_test_link_llvm_jit(test_render_ffi_hotpath)
add_dependencies(all_test_issue_targets test_render_ffi_hotpath)

# Issue #1355: render-aware lightweight mutation checkpoints
aura_add_issue_test(test_render_mutation_checkpoint)
aura_issue_test_link_llvm_jit(test_render_mutation_checkpoint)
add_dependencies(all_test_issue_targets test_render_mutation_checkpoint)

# Issue #1356: HotTierTable tier-based primitive dispatch
aura_add_issue_test(test_tier_dispatch)
aura_issue_test_link_llvm_jit(test_tier_dispatch)
add_dependencies(all_test_issue_targets test_tier_dispatch)

# Issue #1357: per-prim latency + frame time histogram
aura_add_issue_test(test_render_telemetry)
aura_issue_test_link_llvm_jit(test_render_telemetry)
add_dependencies(all_test_issue_targets test_render_telemetry)

# Issue #1358: cyber_cat end-to-end TUI demo smoke
aura_add_issue_test(test_cyber_cat_smoke)
aura_issue_test_link_llvm_jit(test_cyber_cat_smoke)
add_dependencies(all_test_issue_targets test_cyber_cat_smoke)

# Issue #1359: TLarena 1MB default capacity + graceful OOM
aura_add_issue_test(test_tl_arena_capacity)
aura_issue_test_link_llvm_jit(test_tl_arena_capacity)
add_dependencies(all_test_issue_targets test_tl_arena_capacity)

# Issue #1360: env_frames_ truncate on panic (stable append-only EnvId)
aura_add_issue_test(test_envframe_stableid)
aura_issue_test_link_llvm_jit(test_envframe_stableid)
add_dependencies(all_test_issue_targets test_envframe_stableid)

# Issue #1361: aura_free_closure + closure:free! + ID reuse
aura_add_issue_test(test_closure_free)
aura_issue_test_link_llvm_jit(test_closure_free)
add_dependencies(all_test_issue_targets test_closure_free)

# Issue #1362: compact committed mutation_log_ (prevent long-run leak)
aura_add_issue_test(test_compact_mutation_log)
aura_issue_test_link_llvm_jit(test_compact_mutation_log)
add_dependencies(all_test_issue_targets test_compact_mutation_log)

# Issue #1363: PanicCheckpointGuard RAII wired to Evaluator save/restore
aura_add_issue_test(test_panic_checkpoint_raii)
aura_issue_test_link_llvm_jit(test_panic_checkpoint_raii)
add_dependencies(all_test_issue_targets test_panic_checkpoint_raii)

# Issue #1364: safepoint × mutation telemetry + in_gc_safepoint
aura_add_issue_test(test_safepoint_mutation)
aura_issue_test_link_llvm_jit(test_safepoint_mutation)
add_dependencies(all_test_issue_targets test_safepoint_mutation)

# Issue #1365: Closure.bridge_epoch stamp + strict is_bridge_stale
aura_add_issue_test(test_bridge_epoch_strict)
aura_issue_test_link_llvm_jit(test_bridge_epoch_strict)
add_dependencies(all_test_issue_targets test_bridge_epoch_strict)

# Issue #1366: (aot:reload) Aura primitive wrappers for hot-reload
aura_add_issue_test(test_aot_reload_primitive)
aura_issue_test_link_llvm_jit(test_aot_reload_primitive)
add_dependencies(all_test_issue_targets test_aot_reload_primitive)

# Issue #1367: per-evaluator AOT region / module isolation
aura_add_issue_test(test_aot_region_per_eval)
aura_issue_test_link_llvm_jit(test_aot_region_per_eval)
add_dependencies(all_test_issue_targets test_aot_region_per_eval)

# Issue #1368: lazy g_aot_metrics from Evaluator.compiler_metrics_
aura_add_issue_test(test_aot_metrics_lazy)
aura_issue_test_link_llvm_jit(test_aot_metrics_lazy)
add_dependencies(all_test_issue_targets test_aot_metrics_lazy)

# Issue #1369: __top__ always _vN + per-function version probe
aura_add_issue_test(test_aot_mangle_top)
aura_issue_test_link_llvm_jit(test_aot_mangle_top)
add_dependencies(all_test_issue_targets test_aot_mangle_top)

# Issue #1370: lib/std/hot-update Aura stdlib
aura_add_issue_test(test_hot_update_stdlib)
aura_issue_test_link_llvm_jit(test_hot_update_stdlib)
add_dependencies(all_test_issue_targets test_hot_update_stdlib)

# Issue #1371: tag_arity_index_ unordered_map + delta path
aura_add_issue_test(test_tag_arity_index_perf)
aura_issue_test_link_llvm_jit(test_tag_arity_index_perf)
add_dependencies(all_test_issue_targets test_tag_arity_index_perf)

aura_add_issue_test(test_domain_fiber_orchestration)
aura_issue_test_link_llvm_jit(test_domain_fiber_orchestration)
add_dependencies(all_test_issue_targets test_domain_fiber_orchestration)

aura_add_issue_test(test_domain_hygiene_dirty)
aura_issue_test_link_llvm_jit(test_domain_hygiene_dirty)
add_dependencies(all_test_issue_targets test_domain_hygiene_dirty)

aura_add_issue_test(test_domain_typed_mutate)
aura_issue_test_link_llvm_jit(test_domain_typed_mutate)
add_dependencies(all_test_issue_targets test_domain_typed_mutate)
