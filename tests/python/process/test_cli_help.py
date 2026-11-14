"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "cli-help"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "cli_help",
    "cli_version",
    "cli_evo_help",
    "cli_new_run",
    "cli_agent_dialogue",
    "cli_first_screen",
    "cli_source_worldline",
    "cli_build_info",
    "cli_mode_order",
    "cli_tool_file",
)
CASES = (
    (
        4935,
        "test_new_help_creates_nothing",
        "cli_help.Help.test_new_help_creates_nothing",
    ),
    (
        4935,
        "test_every_subcommand_has_help",
        "cli_help.Help.test_every_subcommand_has_help",
    ),
    (
        4935,
        "test_evo_dx_help_snapshot",
        "cli_help.Help.test_evo_dx_help_snapshot",
    ),
    (
        4935,
        "test_unknown_command",
        "cli_help.Help.test_unknown_command",
    ),
    (
        4935,
        "test_top_help_concise_no_path",
        "cli_help.Help.test_top_help_concise_no_path",
    ),
    (
        4934,
        "test_both_spellings",
        "cli_version.Version.test_both_spellings",
    ),
    (
        4948,
        "test_every_module_help_is_pinned",
        "cli_evo_help.ModuleHelp.test_every_module_help_is_pinned",
    ),
    (
        4948,
        "test_every_module_is_in_the_index",
        "cli_evo_help.ModuleHelp.test_every_module_is_in_the_index",
    ),
    (
        4948,
        "test_type_signature_has_help",
        "cli_evo_help.ModuleHelp.test_type_signature_has_help",
    ),
    (
        4948,
        "test_cli_help_is_pinned",
        "cli_evo_help.CliHelp.test_cli_help_is_pinned",
    ),
    (
        4948,
        "test_help_names_the_new_surfaces",
        "cli_evo_help.CliHelp.test_help_names_the_new_surfaces",
    ),
    (
        4913,
        "test_app",
        "cli_new_run.FrontDoor.test_app",
    ),
    (
        4913,
        "test_channel_rule_rejects_unpublished_spellings",
        "cli_new_run.FrontDoor.test_channel_rule_rejects_unpublished_spellings",
    ),
    (
        4913,
        "test_ci_image_is_a_published_channel",
        "cli_new_run.FrontDoor.test_ci_image_is_a_published_channel",
    ),
    (
        4913,
        "test_evolve_worldline_under_5s",
        "cli_new_run.FrontDoor.test_evolve_worldline_under_5s",
    ),
    (
        4913,
        "test_evolve_moo_reports_front_and_qd",
        "cli_new_run.FrontDoor.test_evolve_moo_reports_front_and_qd",
    ),
    (
        4913,
        "test_agent",
        "cli_new_run.FrontDoor.test_agent",
    ),
    (
        4913,
        "test_default_template_and_errors",
        "cli_new_run.FrontDoor.test_default_template_and_errors",
    ),
    (
        4913,
        "test_golden_diff_and_update",
        "cli_new_run.FrontDoor.test_golden_diff_and_update",
    ),
    (
        4913,
        "test_help_lists_commands",
        "cli_new_run.FrontDoor.test_help_lists_commands",
    ),
    (
        4913,
        "test_soft_load",
        "cli_new_run.FrontDoor.test_soft_load",
    ),
    (
        4913,
        "test_embedded_templates_current",
        "cli_new_run.FrontDoor.test_embedded_templates_current",
    ),
    (
        5024,
        "test_demo_agent",
        "cli_agent_dialogue.Dialogue.test_demo_agent",
    ),
    (
        5024,
        "test_new_agent_matches_demo",
        "cli_agent_dialogue.Dialogue.test_new_agent_matches_demo",
    ),
    (
        5024,
        "test_rejected_definition_is_not_live",
        "cli_agent_dialogue.Dialogue.test_rejected_definition_is_not_live",
    ),
    (
        5025,
        "test_docs_start_with_demo_and_the_record",
        "cli_first_screen.FirstScreen.test_docs_start_with_demo_and_the_record",
    ),
    (
        5025,
        "test_new_help_describes_keep_rollback",
        "cli_first_screen.FirstScreen.test_new_help_describes_keep_rollback",
    ),
    (
        5025,
        "test_top_help_first_example_is_demo",
        "cli_first_screen.FirstScreen.test_top_help_first_example_is_demo",
    ),
    (
        5025,
        "test_demo_help",
        "cli_first_screen.FirstScreen.test_demo_help",
    ),
    (
        5026,
        "test_demo",
        "cli_source_worldline.Worldline.test_demo",
    ),
    (
        5026,
        "test_new_evolve_matches_demo",
        "cli_source_worldline.Worldline.test_new_evolve_matches_demo",
    ),
    (
        5026,
        "test_step_three_rolls_back_to_the_last_keep",
        "cli_source_worldline.Worldline.test_step_three_rolls_back_to_the_last_keep",
    ),
    (
        5067,
        "test_env_wins_over_git",
        "cli_build_info.ResolveGitSha.test_env_wins_over_git",
    ),
    (
        5067,
        "test_checkout_sha_is_full",
        "cli_build_info.ResolveGitSha.test_checkout_sha_is_full",
    ),
    (
        5067,
        "test_required_sha_fails_closed",
        "cli_build_info.ResolveGitSha.test_required_sha_fails_closed",
    ),
    (
        5067,
        "test_scripts_pass_sha_and_image_label",
        "cli_build_info.ResolveGitSha.test_scripts_pass_sha_and_image_label",
    ),
    (
        5170,
        "test_preset_dev_before_serve_grants_write",
        "cli_mode_order.TestIssue5170ModeOrder.test_preset_dev_before_serve_grants_write",
    ),
    (
        5170,
        "test_grant_write_before_serve",
        "cli_mode_order.TestIssue5170ModeOrder.test_grant_write_before_serve",
    ),
    (
        5170,
        "test_quiet_before_serve_drops_the_banner",
        "cli_mode_order.TestIssue5170ModeOrder.test_quiet_before_serve_drops_the_banner",
    ),
    (
        5170,
        "test_deny_write_before_serve",
        "cli_mode_order.TestIssue5170ModeOrder.test_deny_write_before_serve",
    ),
    (
        5170,
        "test_serve_before_preset_still_works",
        "cli_mode_order.TestIssue5170ModeOrder.test_serve_before_preset_still_works",
    ),
    (
        5170,
        "test_two_modes_conflict",
        "cli_mode_order.TestIssue5170ModeOrder.test_two_modes_conflict",
    ),
    (
        5170,
        "test_other_tool_modes_follow_options",
        "cli_mode_order.TestIssue5170ModeOrder.test_other_tool_modes_follow_options",
    ),
    (
        5170,
        "test_help_names_both_orders",
        "cli_mode_order.TestIssue5170ModeOrder.test_help_names_both_orders",
    ),
    (
        5170,
        "test_oneshot_without_a_tool_mode",
        "cli_mode_order.TestIssue5170ModeOrder.test_oneshot_without_a_tool_mode",
    ),
    (
        5170,
        "test_load_after_preset_stays_oneshot",
        "cli_mode_order.TestIssue5170ModeOrder.test_load_after_preset_stays_oneshot",
    ),
    (
        5170,
        "test_worker_threads_before_serve",
        "cli_mode_order.TestIssue5170ModeOrder.test_worker_threads_before_serve",
    ),
    (
        5171,
        "test_typecheck_reads_the_file",
        "cli_tool_file.TestIssue5171ToolFile.test_typecheck_reads_the_file",
    ),
    (
        5171,
        "test_source_text_and_missing_paths",
        "cli_tool_file.TestIssue5171ToolFile.test_source_text_and_missing_paths",
    ),
    (
        5171,
        "test_query_reads_the_file",
        "cli_tool_file.TestIssue5171ToolFile.test_query_reads_the_file",
    ),
    (
        5171,
        "test_ir_auto_fix_fmt_and_unparse_agree",
        "cli_tool_file.TestIssue5171ToolFile.test_ir_auto_fix_fmt_and_unparse_agree",
    ),
    (
        5171,
        "test_help_states_the_rule",
        "cli_tool_file.TestIssue5171ToolFile.test_help_states_the_rule",
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
