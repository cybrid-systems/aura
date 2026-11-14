"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "cross-mode"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 1200
PARTS = (
    "xmode_corpus",
    "xmode_force_soa",
    "xmode_force_soa_split",
    "xmode_load_preset",
    "xmode_uncalled",
    "xmode_heap",
    "xmode_statement",
    "xmode_statement_followup",
    "xmode_dotted_rest",
    "xmode_macro_order",
    "xmode_macro_followup",
    "xmode_when_unless",
    "xmode_workflow",
)
CASES = (
    (
        4908,
        "test_corpus_one_result_every_face",
        "xmode_corpus.CrossModeConsistency.test_corpus_one_result_every_face",
    ),
    (
        4908,
        "test_evolve_strategy_summary_every_face",
        "xmode_corpus.CrossModeConsistency.test_evolve_strategy_summary_every_face",
    ),
    (
        4865,
        "test_fiber_set_and_channel_visible",
        "xmode_force_soa.TestForceSoaCells4865.test_fiber_set_and_channel_visible",
    ),
    (
        4865,
        "test_inline_lambda_set",
        "xmode_force_soa.TestForceSoaCells4865.test_inline_lambda_set",
    ),
    (
        4865,
        "test_4842_scope_in_agent",
        "xmode_force_soa.TestForceSoaCells4865.test_4842_scope_in_agent",
    ),
    (
        4865,
        "test_direct_run_force_soa_side_effects",
        "xmode_force_soa.TestForceSoaCells4865.test_direct_run_force_soa_side_effects",
    ),
    (
        4865,
        "test_direct_run_4842_scope_in_agent",
        "xmode_force_soa.TestForceSoaCells4865.test_direct_run_4842_scope_in_agent",
    ),
    (
        4865,
        "test_direct_run_toplevel_while_stays_on_ir_4892",
        "xmode_force_soa.TestForceSoaCells4865.test_direct_run_toplevel_while_stays_on_ir_4892",
    ),
    (
        4865,
        "test_direct_run_walker_bound_value_visible_to_ir_4892",
        "xmode_force_soa.TestForceSoaCells4865.test_direct_run_walker_bound_value_visible_to_ir_4892",
    ),
    (
        4865,
        "test_direct_run_wrapped_form_diag_column_4892",
        "xmode_force_soa.TestForceSoaCells4865.test_direct_run_wrapped_form_diag_column_4892",
    ),
    (
        4869,
        "test_character_literals_are_not_syntax",
        "xmode_force_soa_split.TestForceSoaSplit4869.test_character_literals_are_not_syntax",
    ),
    (
        4869,
        "test_quoted_top_level_form_is_data",
        "xmode_force_soa_split.TestForceSoaSplit4869.test_quoted_top_level_form_is_data",
    ),
    (
        4869,
        "test_unclosed_list_fails_loudly",
        "xmode_force_soa_split.TestForceSoaSplit4869.test_unclosed_list_fails_loudly",
    ),
    (
        4869,
        "test_block_comment_is_skipped",
        "xmode_force_soa_split.TestForceSoaSplit4869.test_block_comment_is_skipped",
    ),
    (
        4869,
        "test_no_extra_last_value",
        "xmode_force_soa_split.TestForceSoaSplit4869.test_no_extra_last_value",
    ),
    (
        4908,
        "test_toml_next_to_entry",
        "xmode_load_preset.LoadPreset.test_toml_next_to_entry",
    ),
    (
        4908,
        "test_toml_next_to_entry_force_soa",
        "xmode_load_preset.LoadPreset.test_toml_next_to_entry_force_soa",
    ),
    (
        4908,
        "test_env_preset",
        "xmode_load_preset.LoadPreset.test_env_preset",
    ),
    (
        4908,
        "test_no_preset_still_denied",
        "xmode_load_preset.LoadPreset.test_no_preset_still_denied",
    ),
    (
        4908,
        "test_intermediate_closure_is_silent",
        "xmode_uncalled.UncalledWarning.test_intermediate_closure_is_silent",
    ),
    (
        4908,
        "test_last_closure_still_warns",
        "xmode_uncalled.UncalledWarning.test_last_closure_still_warns",
    ),
    (
        4908,
        "test_force_soa_restricted_matches_off",
        "xmode_heap.HeapMutateThrottle.test_force_soa_restricted_matches_off",
    ),
    (
        4899,
        "test_vector_ref_statement_position",
        "xmode_statement.TestStatementPositionErrors4899.test_vector_ref_statement_position",
    ),
    (
        4899,
        "test_car_statement_position",
        "xmode_statement.TestStatementPositionErrors4899.test_car_statement_position",
    ),
    (
        4899,
        "test_error_statement_position",
        "xmode_statement.TestStatementPositionErrors4899.test_error_statement_position",
    ),
    (
        4899,
        "test_nested_vector_ref_statement_position",
        "xmode_statement.TestStatementPositionErrors4899.test_nested_vector_ref_statement_position",
    ),
    (
        4899,
        "test_value_position_still_reports",
        "xmode_statement.TestStatementPositionErrors4899.test_value_position_still_reports",
    ),
    (
        4899,
        "test_try_catch_still_handles_statement_errors",
        "xmode_statement.TestStatementPositionErrors4899.test_try_catch_still_handles_statement_errors",
    ),
    (
        4899,
        "test_value_define_error",
        "xmode_statement_followup.TestTopLevelErrorsEveryFace4899.test_value_define_error",
    ),
    (
        4899,
        "test_function_body_error",
        "xmode_statement_followup.TestTopLevelErrorsEveryFace4899.test_function_body_error",
    ),
    (
        4899,
        "test_error_call_in_function",
        "xmode_statement_followup.TestTopLevelErrorsEveryFace4899.test_error_call_in_function",
    ),
    (
        4899,
        "test_while_body_error",
        "xmode_statement_followup.TestTopLevelErrorsEveryFace4899.test_while_body_error",
    ),
    (
        4899,
        "test_function_body_error_mid_file_runs_forms_once",
        "xmode_statement_followup.TestTopLevelErrorsEveryFace4899.test_function_body_error_mid_file_runs_forms_once",
    ),
    (
        4899,
        "test_tail_error_located_and_not_rerun",
        "xmode_statement_followup.TestTopLevelErrorsEveryFace4899.test_tail_error_located_and_not_rerun",
    ),
    (
        4900,
        "test_rest_binding_matches_static_face",
        "xmode_dotted_rest.TestForceSoaRestBinding4900.test_rest_binding_matches_static_face",
    ),
    (
        4900,
        "test_single_toplevel_call",
        "xmode_dotted_rest.TestForceSoaRestBinding4900.test_single_toplevel_call",
    ),
    (
        4901,
        "test_each_call_uses_the_previous_definition",
        "xmode_macro_order.TestMacroRedefinitionOrder4901.test_each_call_uses_the_previous_definition",
    ),
    (
        4901,
        "test_all_faces_agree",
        "xmode_macro_followup.TestForceSoaMacroView4901.test_all_faces_agree",
    ),
    (
        4903,
        "test_when_unless_all_faces",
        "xmode_when_unless.TestTopLevelWhenUnless4903.test_when_unless_all_faces",
    ),
    (
        4903,
        "test_named_let_loop_terminates",
        "xmode_when_unless.TestTopLevelWhenUnless4903.test_named_let_loop_terminates",
    ),
    (
        4903,
        "test_body_error_still_reported",
        "xmode_when_unless.TestTopLevelWhenUnless4903.test_body_error_still_reported",
    ),
    (
        4904,
        "test_all_faces",
        "xmode_workflow.TestForceSoaWorkflow4904.test_all_faces",
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
