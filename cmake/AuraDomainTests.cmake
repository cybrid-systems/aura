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

aura_add_issue_test(test_domain_fiber_orchestration)
aura_issue_test_link_llvm_jit(test_domain_fiber_orchestration)
add_dependencies(all_test_issue_targets test_domain_fiber_orchestration)

aura_add_issue_test(test_domain_hygiene_dirty)
aura_issue_test_link_llvm_jit(test_domain_hygiene_dirty)
add_dependencies(all_test_issue_targets test_domain_hygiene_dirty)

aura_add_issue_test(test_domain_typed_mutate)
aura_issue_test_link_llvm_jit(test_domain_typed_mutate)
add_dependencies(all_test_issue_targets test_domain_typed_mutate)
