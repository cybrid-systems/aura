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

# Issue #1559: present_batch / draw_batch engine + dirty short-circuit
aura_add_issue_test(test_render_primitives)
# Pure C++ engine test — no CompilerService / LLVM required.
add_dependencies(all_test_issue_targets test_render_primitives)

# Issue #1561: Arena-backed zero-copy views + present_batch integration
aura_add_issue_test(test_zero_copy_arena)
aura_issue_test_link_llvm_jit(test_zero_copy_arena)
add_dependencies(all_test_issue_targets test_zero_copy_arena)

# Issue #1562: dirty-region differential present / batch_terminal delta
aura_add_issue_test(test_dirty_delta_present)
aura_issue_test_link_llvm_jit(test_dirty_delta_present)
add_dependencies(all_test_issue_targets test_dirty_delta_present)

# Issue #1563: render_critical deopt throttle under mutation pressure
aura_add_issue_test(test_render_hotpath_stability_under_mutation)
aura_issue_test_link_llvm_jit(test_render_hotpath_stability_under_mutation)
add_dependencies(all_test_issue_targets test_render_hotpath_stability_under_mutation)

# Issue #1564: full StableNodeRef provenance enforcement across paths
aura_add_issue_test(test_stable_ref_full_provenance_enforcement)
aura_issue_test_link_llvm_jit(test_stable_ref_full_provenance_enforcement)
add_dependencies(all_test_issue_targets test_stable_ref_full_provenance_enforcement)

# Issue #1565: Capability Effects — check_and_record_effect + Strict deny
aura_add_issue_test(test_capability_effects_enforcement)
aura_issue_test_link_llvm_jit(test_capability_effects_enforcement)
add_dependencies(all_test_issue_targets test_capability_effects_enforcement)

# Issue #1566: multi-tenant WorkspaceIsolationPolicy + provenance
aura_add_issue_test(test_tenant_isolation_enforcement)
aura_issue_test_link_llvm_jit(test_tenant_isolation_enforcement)
add_dependencies(all_test_issue_targets test_tenant_isolation_enforcement)

# Issue #1567: mutation audit WAL persist + crash recovery
aura_add_issue_test(test_mutation_audit_wal)
aura_issue_test_link_llvm_jit(test_mutation_audit_wal)
add_dependencies(all_test_issue_targets test_mutation_audit_wal)

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

# Issue #1454: aura-pets multi-demo headless TUI regression (also in issues_fast)
aura_add_issue_test(test_aura_pets_smoke)
aura_issue_test_link_llvm_jit(test_aura_pets_smoke)
add_dependencies(all_test_issue_targets test_aura_pets_smoke)

# Issue #1359: TLarena 1MB default capacity + graceful OOM
aura_add_issue_test(test_tl_arena_capacity)
aura_issue_test_link_llvm_jit(test_tl_arena_capacity)
add_dependencies(all_test_issue_targets test_tl_arena_capacity)

# Issue #1360: env_frames_ truncate on panic (stable append-only EnvId)
aura_add_issue_test(test_envframe_stableid)
aura_issue_test_link_llvm_jit(test_envframe_stableid)
add_dependencies(all_test_issue_targets test_envframe_stableid)

# Issue #1382: ASTArena contract test — run_destructors() must run
# before resource_.release() in ~ASTArena() and reset(). Uses a
# counting memory_resource as the upstream to detect release()
# ordering. No LLVM JIT needed (pure C++ arena contract test).
aura_add_issue_test(test_issue_1382_arena_dtor_order)
add_dependencies(all_test_issue_targets test_issue_1382_arena_dtor_order)

# Issue #1383: throttled warning when InvariantCheckMode::Disabled
# is set on a workspace with mutation_history > 0. Verifies the
# warning is emitted exactly once per mode flip (not per-mutation
# spam) and includes the current mutation count. Uses
# aura_issue_test_link_llvm_jit_minimal because typed_mutate
# touches g_tl_arena / aura_set_aot_metrics symbols.
aura_add_issue_test(test_issue_1383_disabled_mode_warn)
aura_issue_test_link_llvm_jit_minimal(test_issue_1383_disabled_mode_warn)
add_dependencies(all_test_issue_targets test_issue_1383_disabled_mode_warn)

# Issue #1387: validate_ownership_full discovers type-driven
# linear bindings via reg.linear_of(type_id) alongside the
# existing syntactic Linear wrapper check (set union, defense
# in depth). Requires the new const TypeRegistry& parameter
# (old 3-arg callers fail to compile).
aura_add_issue_test(test_issue_1387_type_driven_linear)
add_dependencies(all_test_issue_targets test_issue_1387_type_driven_linear)

# Issue #1388: lock hierarchy contract test + 4 mutex decl
# acquire-order doc. Uses mini-stubs of the 4 production locks
# (mutate/workspace/env_frames/dep_graph) to verify the canonical
# acquire order under concurrent workload. llvm_jit_minimal
# sufficient (no Aura runtime symbols needed for stub-based test).
aura_add_issue_test(test_lock_hierarchy)
aura_issue_test_link_llvm_jit_minimal(test_lock_hierarchy)
add_dependencies(all_test_issue_targets test_lock_hierarchy)

# Issue #1389: query_mutation_log iter+append race contract
# test. Verifies that query_mutation_log() acquires workspace
# shared_lock during the mutation_log_ copy (prevents UB when
# concurrent typed_mutate push_back invalidates the iterator).
# Thread A calls query_mutation_log() directly via C++; Thread B
# does Aura (set! ...) loop that triggers typed_mutate internally.
# Real race at workspace_mtx_ — no Aura serialization involved.
aura_add_issue_test(test_mutation_log_query_race)
aura_issue_test_link_llvm_jit_minimal(test_mutation_log_query_race)
add_dependencies(all_test_issue_targets test_mutation_log_query_race)

# Issue #1390: request_defrag feedback + defrag×alloc contract
# test. Verifies the new bool return on request_defrag(), the
# (arena:safepoint-registered?) and (arena:warn-no-safepoint)
# primitives, and that concurrent arena.create<T>() + defrag()
# runs without UB. llvm_jit_minimal sufficient (uses Aura
# primitives + direct C++ arena API).
aura_add_issue_test(test_arena_defrag_concurrent)
aura_issue_test_link_llvm_jit_minimal(test_arena_defrag_concurrent)
add_dependencies(all_test_issue_targets test_arena_defrag_concurrent)

# Issue #1391: apply_closure C++ recursion safety contract
# test. Verifies the Issue #109 thread_local depth guard
# (MAX_C_STACK_DEPTH=700 in evaluator_eval_flat.cpp:1698-1729)
# catches deep recursion gracefully (no SIGSEGV), and that
# shallow TCO + closure semantics still work. Tests the
# cross-closure-boundary eval_flat → apply_closure → eval_flat
# recursion path (separate from intra-eval_flat TCO trampoline
# at line 2971/2979/3347/3372). llvm_jit_minimal sufficient
# (Aura eval path, no JIT/AOT needed).
aura_add_issue_test(test_issue_1391_apply_closure_recursion)
aura_issue_test_link_llvm_jit_minimal(test_issue_1391_apply_closure_recursion)
add_dependencies(all_test_issue_targets test_issue_1391_apply_closure_recursion)

# Issue #1392: macro hygiene depth observability contract.
# Verifies (compile:macro-origin-provenance-errors) primitive
# works (returns g_macro_origin_provenance_errors counter value)
# and that MAX_HYGIENE_DEPTH was raised from 256 to 1024.
# Smoke test — depth > 1024 triggers the counter increment but
# writing a 1024-level nested macro is impractical in a single
# test; observability plumbing is the scope-limited fix.
aura_add_issue_test(test_issue_1392_macro_hygiene_depth)
aura_issue_test_link_llvm_jit_minimal(test_issue_1392_macro_hygiene_depth)
add_dependencies(all_test_issue_targets test_issue_1392_macro_hygiene_depth)

# Issue #1393: PanicCheckpoint cross-evaluator discriminator
# contract test. Verifies PanicCheckpointHost's new
# expected_evaluator_id field + Guard dtor's mismatch check
# (skips restore + bumps restores_discriminator_failed counter
# when ctx != expected_evaluator_id). Tests cover AC1 (mismatch
# bumps counter), AC2 (matching preserves normal restore flow),
# AC3 (field exposed). llvm_jit_minimal sufficient — uses core
# type-erased host API directly, no Aura compiler needed.
aura_add_issue_test(test_issue_1393_panic_checkpoint_cross_evaluator)
aura_issue_test_link_llvm_jit_minimal(test_issue_1393_panic_checkpoint_cross_evaluator)
add_dependencies(all_test_issue_targets test_issue_1393_panic_checkpoint_cross_evaluator)

# Issue #1394: EvalValue string v2 encoding round-trip regression.
# Verifies make_string(N) + is_string + as_string_idx round-trips
# correctly for collision-sensitive indices (31, 19, 95, 83) that
# would have hit RefError/RefKeyword collisions under v1 encoding.
# Also tests 0..1023 random indices for sanity + is_string
# classification at low indices. llvm_jit_minimal sufficient —
# exercises the value module directly, no Aura runtime needed.
aura_add_issue_test(test_issue_1394_value_string_v2_round_trip)
aura_issue_test_link_llvm_jit_minimal(test_issue_1394_value_string_v2_round_trip)
add_dependencies(all_test_issue_targets test_issue_1394_value_string_v2_round_trip)

# Issue #1395: compile:mark-dirty! primitives capability gate
# contract test. Verifies the 4 newly-gated primitives
# (mark-instruction-dirty!, clear-instruction-dirty!,
# mark-dirty-upward-fast, clear-macro-dirty!) return merr
# when called without kCapWildcard in sandbox_mode, and work
# when called without sandbox_mode. Also verifies the 3
# #1293-gated primitives (kCapCompileDirty/Deopt) are
# unchanged (backward compat).
aura_add_issue_test(test_issue_1395_dirty_primitives_cap_gate)
aura_issue_test_link_llvm_jit_minimal(test_issue_1395_dirty_primitives_cap_gate)
add_dependencies(all_test_issue_targets test_issue_1395_dirty_primitives_cap_gate)

# Issue #1384: EnvFrame::version_ must be initialized to the
# current defuse_version_ BEFORE push_back (not via default ctor
# + post-fill), so concurrent readers never observe version_ ==
# 0 inside the unique_lock window. Also re-stamps version_ in
# alloc_env_frame_from_env after bindings are assigned so the
# frame captures defuse_version_ at COMPLETION.
aura_add_issue_test(test_issue_1384_envframe_version_init)
aura_issue_test_link_llvm_jit_minimal(test_issue_1384_envframe_version_init)
add_dependencies(all_test_issue_targets test_issue_1384_envframe_version_init)

# Issue #1385: env_frames_ + arena observability via
# (compiler:metrics) primitive (returns JSON with the 4 keys:
# env_frames_size_total, env_frames_stale_count,
# ast_arena_bytes_in_use, ast_arena_upstream_bytes). The arena
# uses a CountingMR as its upstream so fallback bytes are
# observable. Uses aura_issue_test_link_llvm_jit_minimal because
# the test path touches g_tl_arena / jit runtime symbols.
aura_add_issue_test(test_issue_1385_env_arena_metrics)
aura_issue_test_link_llvm_jit_minimal(test_issue_1385_env_arena_metrics)
add_dependencies(all_test_issue_targets test_issue_1385_env_arena_metrics)

# Issue #1386: env_frames_ arena compaction + Closure::env_id
# rewrite via remap table. Uses (evaluator:compact-env-frames)
# primitive + (compiler:metrics) to verify reclamation + env_id
# rewrite correctness. Needs full llvm_jit link (not minimal)
# because AC3 actually invokes a closure via (c) — closure
# materialization path requires full JIT runtime. Same link
# pattern as test_issues_809_817_batch / test_issues_819_829_batch.
aura_add_issue_test(test_issue_1386_compact_env_frames)
aura_issue_test_link_llvm_jit(test_issue_1386_compact_env_frames)
add_dependencies(all_test_issue_targets test_issue_1386_compact_env_frames)

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

# Issue #1372: close query:pattern tag_arity race window
aura_add_issue_test(test_query_pattern_concurrent)
aura_issue_test_link_llvm_jit(test_query_pattern_concurrent)
add_dependencies(all_test_issue_targets test_query_pattern_concurrent)

# Issue #1373: mutation boundary hold + cross-thread migration counters
aura_add_issue_test(test_mutate_cross_thread_migration)
aura_issue_test_link_llvm_jit(test_mutate_cross_thread_migration)
add_dependencies(all_test_issue_targets test_mutate_cross_thread_migration)

# Issue #1374: query:pattern ↔ mutate:replace-pattern default Kleene parity
aura_add_issue_test(test_query_mutate_consistency)
aura_issue_test_link_llvm_jit(test_query_mutate_consistency)
add_dependencies(all_test_issue_targets test_query_mutate_consistency)

# Issue #1375: MutationBoundaryGuard hold-time histogram
aura_add_issue_test(test_mutation_hold_time)
aura_issue_test_link_llvm_jit(test_mutation_hold_time)
add_dependencies(all_test_issue_targets test_mutation_hold_time)

# Issue #1376: dep_graph_ mutex + concurrent record_dependency
aura_add_issue_test(test_dep_graph_concurrent)
aura_issue_test_link_llvm_jit(test_dep_graph_concurrent)
add_dependencies(all_test_issue_targets test_dep_graph_concurrent)

# Issue #1377: SoA dual-emit opt-in gate (default off)
aura_add_issue_test(test_ir_soa_dual_emit)
aura_issue_test_link_llvm_jit(test_ir_soa_dual_emit)
add_dependencies(all_test_issue_targets test_ir_soa_dual_emit)

# Issue #1378: invalidate_function cascade ordering under mutate_lock
aura_add_issue_test(test_invalidate_cascade_order)
aura_issue_test_link_llvm_jit(test_invalidate_cascade_order)
add_dependencies(all_test_issue_targets test_invalidate_cascade_order)

# Issue #1380: generic atomic-resource-swap stdlib
aura_add_issue_test(test_atomic_swap_stdlib)
aura_issue_test_link_llvm_jit(test_atomic_swap_stdlib)
add_dependencies(all_test_issue_targets test_atomic_swap_stdlib)

# Issue #1381: workspace binary serialize/deserialize
aura_add_issue_test(test_persist_basic)
aura_issue_test_link_llvm_jit(test_persist_basic)
add_dependencies(all_test_issue_targets test_persist_basic)

aura_add_issue_test(test_domain_fiber_orchestration)
aura_issue_test_link_llvm_jit(test_domain_fiber_orchestration)
add_dependencies(all_test_issue_targets test_domain_fiber_orchestration)

aura_add_issue_test(test_domain_hygiene_dirty)
aura_issue_test_link_llvm_jit(test_domain_hygiene_dirty)
add_dependencies(all_test_issue_targets test_domain_hygiene_dirty)

aura_add_issue_test(test_domain_typed_mutate)
aura_issue_test_link_llvm_jit(test_domain_typed_mutate)
add_dependencies(all_test_issue_targets test_domain_typed_mutate)
