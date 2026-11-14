"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "sandbox"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 360
PARTS = (
    "sand_production",
    "sand_presets",
    "sand_write_scope",
    "sand_soft_rebind",
    "sand_std_require",
    "sand_moving_closure",
    "sand_rebind_restore",
    "sand_rebind_snapshot",
    "sand_std",
    "sand_dev_child",
    "sand_stdlib_unbound",
    "sand_frequencies",
)
CASES = (
    (
        4870,
        "test_agent_and_scope_bodies_run",
        "sand_production.TestProductionSandbox4870.test_agent_and_scope_bodies_run",
    ),
    (
        4870,
        "test_agent_ask_answers",
        "sand_production.TestProductionSandbox4870.test_agent_ask_answers",
    ),
    (
        4912,
        "test_default_denies_with_hint",
        "sand_presets.Presets.test_default_denies_with_hint",
    ),
    (
        4912,
        "test_dev_runs_self_evolving_program",
        "sand_presets.Presets.test_dev_runs_self_evolving_program",
    ),
    (
        4912,
        "test_quiet_drops_banner",
        "sand_presets.Presets.test_quiet_drops_banner",
    ),
    (
        4912,
        "test_env_and_toml",
        "sand_presets.Presets.test_env_and_toml",
    ),
    (
        4912,
        "test_grant_and_deny",
        "sand_presets.Presets.test_grant_and_deny",
    ),
    (
        4912,
        "test_prod_is_default_and_rejects_grant",
        "sand_presets.Presets.test_prod_is_default_and_rejects_grant",
    ),
    (
        4912,
        "test_bad_values",
        "sand_presets.Presets.test_bad_values",
    ),
    (
        4912,
        "test_preset_under_sandbox_off",
        "sand_presets.Presets.test_preset_under_sandbox_off",
    ),
    (
        4912,
        "test_grant_write_dir_all_pipelines",
        "sand_write_scope.WriteScope.test_grant_write_dir_all_pipelines",
    ),
    (
        4912,
        "test_ci_preset_scopes_to_out",
        "sand_write_scope.WriteScope.test_ci_preset_scopes_to_out",
    ),
    (
        4912,
        "test_denial_is_classified_with_hint",
        "sand_write_scope.WriteScope.test_denial_is_classified_with_hint",
    ),
    (
        4912,
        "test_project_run_ci_uses_toml_out",
        "sand_write_scope.WriteScope.test_project_run_ci_uses_toml_out",
    ),
    (
        4912,
        "test_dev_and_bare_write_are_unscoped",
        "sand_write_scope.WriteScope.test_dev_and_bare_write_are_unscoped",
    ),
    (
        4912,
        "test_default_still_denies_write",
        "sand_write_scope.WriteScope.test_default_still_denies_write",
    ),
    (
        4914,
        "test_list_quote_restore",
        "sand_soft_rebind.SoftRebindMatchesOff.test_list_quote_restore",
    ),
    (
        4914,
        "test_alist_and_lambda_data",
        "sand_soft_rebind.SoftRebindMatchesOff.test_alist_and_lambda_data",
    ),
    (
        4914,
        "test_reject_names_reason_and_recovers",
        "sand_soft_rebind.SoftRebindMatchesOff.test_reject_names_reason_and_recovers",
    ),
    (
        4919,
        "test_require_fs_and_read_only_calls",
        "sand_std_require.TestStdRequireNoEffect4919.test_require_fs_and_read_only_calls",
    ),
    (
        4919,
        "test_write_call_denied_with_hint",
        "sand_std_require.TestStdRequireNoEffect4919.test_write_call_denied_with_hint",
    ),
    (
        4919,
        "test_file_copy_needs_write_not_exec",
        "sand_std_require.TestStdRequireNoEffect4919.test_file_copy_needs_write_not_exec",
    ),
    (
        4919,
        "test_shell_denied_at_call",
        "sand_std_require.TestStdRequireNoEffect4919.test_shell_denied_at_call",
    ),
    (
        4919,
        "test_bool_type_with_eq",
        "sand_std_require.TestStdRequireNoEffect4919.test_bool_type_with_eq",
    ),
    (
        4928,
        "test_matrix",
        "sand_moving_closure.MovingClosureVoid.test_matrix",
    ),
    (
        4928,
        "test_mutate_inside_running_closure",
        "sand_moving_closure.MovingClosureVoid.test_mutate_inside_running_closure",
    ),
    (
        4928,
        "test_repl_force_soa",
        "sand_moving_closure.MovingClosureVoid.test_repl_force_soa",
    ),
    (
        4928,
        "test_repl_no_moving_off_default",
        "sand_moving_closure.MovingClosureVoid.test_repl_no_moving_off_default",
    ),
    (
        4937,
        "test_restore_matrix",
        "sand_rebind_restore.RebindRestore4937.test_restore_matrix",
    ),
    (
        4937,
        "test_rebind_missing_name_adds",
        "sand_rebind_restore.RebindRestore4937.test_rebind_missing_name_adds",
    ),
    (
        4937,
        "test_repro_all_modes",
        "sand_rebind_snapshot.RebindSnapshotMember.test_repro_all_modes",
    ),
    (
        4937,
        "test_missing_name_adds_and_survives_restore",
        "sand_rebind_snapshot.RebindSnapshotMember.test_missing_name_adds_and_survives_restore",
    ),
    (
        4937,
        "test_existing_name_member_body",
        "sand_rebind_snapshot.RebindSnapshotMember.test_existing_name_member_body",
    ),
    (
        4937,
        "test_failed_rebind_still_rolls_back",
        "sand_rebind_snapshot.RebindSnapshotMember.test_failed_rebind_still_rolls_back",
    ),
    (
        5136,
        "test_default_face_denies_and_stays_catchable",
        "sand_std.StdSandboxPreset.test_default_face_denies_and_stays_catchable",
    ),
    (
        5136,
        "test_sandbox_off_allows_the_same_require",
        "sand_std.StdSandboxPreset.test_sandbox_off_allows_the_same_require",
    ),
    (
        5287,
        "test_missing_network_names_the_grant",
        "sand_std.LlmChatNetworkGrant.test_missing_network_names_the_grant",
    ),
    (
        5287,
        "test_grant_or_sandbox_off_does_not_ask_for_network",
        "sand_std.LlmChatNetworkGrant.test_grant_or_sandbox_off_does_not_ask_for_network",
    ),
    (
        5086,
        "test_preset_dev_trial_mutates_child_not_live",
        "sand_dev_child.DevChildCow.test_preset_dev_trial_mutates_child_not_live",
    ),
    (
        5169,
        "test_checker_accepts_tree",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_checker_accepts_tree",
    ),
    (
        5169,
        "test_checker_flags_a_missing_qualified_call",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_checker_flags_a_missing_qualified_call",
    ),
    (
        5169,
        "test_engine_catalog_skips_stats_only_getters",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_engine_catalog_skips_stats_only_getters",
    ),
    (
        5169,
        "test_refactor_inline_counts_callers",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_refactor_inline_counts_callers",
    ),
    (
        5169,
        "test_strategy_non_string_uses_best",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_strategy_non_string_uses_best",
    ),
    (
        5169,
        "test_hot_update_reads_stats",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_hot_update_reads_stats",
    ),
    (
        5169,
        "test_region_get_reads_stats",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_region_get_reads_stats",
    ),
    (
        5169,
        "test_check_success_reports_a_match",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_check_success_reports_a_match",
    ),
    (
        5169,
        "test_heal_diagnose_fallback_is_bound",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_heal_diagnose_fallback_is_bound",
    ),
    (
        5169,
        "test_sources_call_real_names",
        "sand_stdlib_unbound.TestIssue5169StdlibUnbound.test_sources_call_real_names",
    ),
    (
        0,
        "test_hash_then_iter_is_a_hash",
        "sand_frequencies.TestFrequenciesRestricted.test_hash_then_iter_is_a_hash",
    ),
    (
        0,
        "test_iter_then_hash_is_a_hash",
        "sand_frequencies.TestFrequenciesRestricted.test_iter_then_hash_is_a_hash",
    ),
    (
        0,
        "test_local_let_hash_set_and_published_still_denied",
        "sand_frequencies.TestFrequenciesRestricted.test_local_let_hash_set_and_published_still_denied",
    ),
)


def load_tests(loader, tests, pattern):
    import importlib
    import sys

    suite = unittest.TestSuite()
    for _issue, _title, spec in CASES:
        bits = spec.split(".")
        if len(bits) == 3:
            mod = importlib.import_module(bits[0])
            suite.addTest(getattr(mod, bits[1])(bits[2]))
        else:
            suite.addTest(getattr(sys.modules[__name__], bits[0])(bits[1]))
    return suite


if __name__ == "__main__":
    unittest.main()
