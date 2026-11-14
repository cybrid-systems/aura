"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "diag-ir"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "diag_define_value_error",
    "diag_first_error",
    "diag_ir_body_loc",
    "diag_ir_bridge",
    "diag_once",
    "diag_snapshots",
    "diag_walker_depth",
)
CASES = (
    (
        4909,
        "test_head_define_error_value_stops",
        "diag_define_value_error.DefineValueError.test_head_define_error_value_stops",
    ),
    (
        4909,
        "test_keep_going_reports_once_and_runs_on",
        "diag_define_value_error.DefineValueError.test_keep_going_reports_once_and_runs_on",
    ),
    (
        4909,
        "test_memq_hint_on_every_face",
        "diag_define_value_error.DefineValueError.test_memq_hint_on_every_face",
    ),
    (
        4909,
        "test_file_stops_at_first_error",
        "diag_first_error.FirstErrorStops.test_file_stops_at_first_error",
    ),
    (
        4909,
        "test_keep_going_flag",
        "diag_first_error.FirstErrorStops.test_keep_going_flag",
    ),
    (
        4909,
        "test_keep_going_env",
        "diag_first_error.FirstErrorStops.test_keep_going_env",
    ),
    (
        4909,
        "test_stdin_keeps_running",
        "diag_first_error.FirstErrorStops.test_stdin_keeps_running",
    ),
    (
        4909,
        "test_query_ops_dispatch_and_reject_unknown",
        "diag_first_error.FirstErrorStops.test_query_ops_dispatch_and_reject_unknown",
    ),
    (
        4909,
        "test_call_statement",
        "diag_ir_body_loc.IrBodyLocation.test_call_statement",
    ),
    (
        4909,
        "test_deny_keeps_its_cause_on_every_face",
        "diag_ir_bridge.IrBridgeDiag.test_deny_keeps_its_cause_on_every_face",
    ),
    (
        4909,
        "test_top_level_type_error_once",
        "diag_once.ReportedOnceWithLocation.test_top_level_type_error_once",
    ),
    (
        4909,
        "test_type_error_in_display_once",
        "diag_once.ReportedOnceWithLocation.test_type_error_in_display_once",
    ),
    (
        4909,
        "test_primitive_error_located",
        "diag_once.ReportedOnceWithLocation.test_primitive_error_located",
    ),
    (
        4909,
        "test_cdr_error_in_display_located",
        "diag_once.ReportedOnceWithLocation.test_cdr_error_in_display_located",
    ),
    (
        4909,
        "test_bare_primitive_error_located",
        "diag_once.ReportedOnceWithLocation.test_bare_primitive_error_located",
    ),
    (
        4909,
        "test_discarded_error_in_body_still_reported_once",
        "diag_once.ReportedOnceWithLocation.test_discarded_error_in_body_still_reported_once",
    ),
    (
        4909,
        "test_displayed_error_in_body_located",
        "diag_once.ReportedOnceWithLocation.test_displayed_error_in_body_located",
    ),
    (
        4909,
        "test_snapshots",
        "diag_snapshots.Snapshots.test_snapshots",
    ),
    (
        4909,
        "test_each_line_once",
        "diag_walker_depth.WalkerDepth.test_each_line_once",
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
