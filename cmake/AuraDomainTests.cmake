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

aura_add_issue_test(test_domain_fiber_orchestration)
aura_issue_test_link_llvm_jit(test_domain_fiber_orchestration)
add_dependencies(all_test_issue_targets test_domain_fiber_orchestration)

aura_add_issue_test(test_domain_hygiene_dirty)
aura_issue_test_link_llvm_jit(test_domain_hygiene_dirty)
add_dependencies(all_test_issue_targets test_domain_hygiene_dirty)

aura_add_issue_test(test_domain_typed_mutate)
aura_issue_test_link_llvm_jit(test_domain_typed_mutate)
add_dependencies(all_test_issue_targets test_domain_typed_mutate)
