"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "serve"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "serve_protocol",
    "serve_preset",
    "serve_edit",
    "serve_error",
    "serve_load_parity",
)
CASES = (
    (
        5158,
        "test_same_client_accepted_set",
        "serve_protocol.ServeProtocol5158.test_same_client_accepted_set",
    ),
    (
        5158,
        "test_help_all_documents_the_protocol",
        "serve_protocol.ServeProtocol5158.test_help_all_documents_the_protocol",
    ),
    (
        5167,
        "test_preset_dev_grants_set_code",
        "serve_preset.TestIssue5167ServePreset.test_preset_dev_grants_set_code",
    ),
    (
        5167,
        "test_preset_dev_display_repro",
        "serve_preset.TestIssue5167ServePreset.test_preset_dev_display_repro",
    ),
    (
        5167,
        "test_grant_mutate",
        "serve_preset.TestIssue5167ServePreset.test_grant_mutate",
    ),
    (
        5167,
        "test_deny_write_keeps_mutate",
        "serve_preset.TestIssue5167ServePreset.test_deny_write_keeps_mutate",
    ),
    (
        5167,
        "test_deny_mutate_still_refuses",
        "serve_preset.TestIssue5167ServePreset.test_deny_mutate_still_refuses",
    ),
    (
        5167,
        "test_plain_serve_stays_restricted",
        "serve_preset.TestIssue5167ServePreset.test_plain_serve_stays_restricted",
    ),
    (
        5168,
        "test_default_face_is_effect_denied",
        "serve_edit.TestIssue5168ServeEditLoop.test_default_face_is_effect_denied",
    ),
    (
        5168,
        "test_preset_dev_runs_the_loop",
        "serve_edit.TestIssue5168ServeEditLoop.test_preset_dev_runs_the_loop",
    ),
    (
        5168,
        "test_plain_display_stays_ok",
        "serve_edit.TestIssue5168ServeEditLoop.test_plain_display_stays_ok",
    ),
    (
        5168,
        "test_pinned_strict_is_user_level",
        "serve_edit.TestIssue5168ServeEditLoop.test_pinned_strict_is_user_level",
    ),
    (
        5168,
        "test_help_names_the_edit_loop",
        "serve_edit.TestIssue5168ServeEditLoop.test_help_names_the_edit_loop",
    ),
    (
        5172,
        "test_car_is_an_error_on_every_sync_path",
        "serve_error.TestIssue5172ServeErrorValue.test_car_is_an_error_on_every_sync_path",
    ),
    (
        5172,
        "test_serve_async_reports_the_same_failure",
        "serve_error.TestIssue5172ServeErrorValue.test_serve_async_reports_the_same_failure",
    ),
    (
        5172,
        "test_string_error_text_stays_ok",
        "serve_error.TestIssue5172ServeErrorValue.test_string_error_text_stays_ok",
    ),
    (
        5172,
        "test_successful_exec_stays_ok",
        "serve_error.TestIssue5172ServeErrorValue.test_successful_exec_stays_ok",
    ),
    (
        4856,
        "test_4856_agent_body_failure_is_not_ok",
        "serve_load_parity.TestIssue4856DualModeConsistency.test_4856_agent_body_failure_is_not_ok",
    ),
    (
        4856,
        "test_4856_scope_join_all_status_matches",
        "serve_load_parity.TestIssue4856DualModeConsistency.test_4856_scope_join_all_status_matches",
    ),
    (
        4856,
        "test_4856_nested_scope_in_agent_body_matches",
        "serve_load_parity.TestIssue4856DualModeConsistency.test_4856_nested_scope_in_agent_body_matches",
    ),
    (
        4856,
        "test_4856_successful_agent_matches",
        "serve_load_parity.TestIssue4856DualModeConsistency.test_4856_successful_agent_matches",
    ),
    (
        4856,
        "test_4856_uncaught_error_keeps_location_and_snippet",
        "serve_load_parity.TestIssue4856DualModeConsistency.test_4856_uncaught_error_keeps_location_and_snippet",
    ),
    (
        4856,
        "test_4856_bench_failure_keeps_the_eval_error_marker",
        "serve_load_parity.TestIssue4856DualModeConsistency.test_4856_bench_failure_keeps_the_eval_error_marker",
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
