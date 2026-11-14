"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "diagnostic-location"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "dloc_file_face",
    "dloc_quality",
    "dloc_module_file",
)
CASES = (
    (
        4864,
        "test_oneshot_file_face_matches_load_face",
        "dloc_file_face.TestIssue4864.test_oneshot_file_face_matches_load_face",
    ),
    (
        4855,
        "test_4855_caught_message_names_operation_and_argument",
        "dloc_quality.TestIssue4855DiagnosticQuality.test_4855_caught_message_names_operation_and_argument",
    ),
    (
        4855,
        "test_4855_uncaught_error_is_reported_once_at_the_inner_form",
        "dloc_quality.TestIssue4855DiagnosticQuality.test_4855_uncaught_error_is_reported_once_at_the_inner_form",
    ),
    (
        4855,
        "test_4855_issue_repro_hash_ref_raises_on_missing_key",
        "dloc_quality.TestIssue4855DiagnosticQuality.test_4855_issue_repro_hash_ref_raises_on_missing_key",
    ),
    (
        4855,
        "test_4855_hash_ref_default_still_returns_the_default",
        "dloc_quality.TestIssue4855DiagnosticQuality.test_4855_hash_ref_default_still_returns_the_default",
    ),
    (
        4855,
        "test_4855_hash_ref_missing_key_is_catchable",
        "dloc_quality.TestIssue4855DiagnosticQuality.test_4855_hash_ref_missing_key_is_catchable",
    ),
    (
        4855,
        "test_4855_ir_executor_comparison_names_the_operation",
        "dloc_quality.TestIssue4855DiagnosticQuality.test_4855_ir_executor_comparison_names_the_operation",
    ),
    (
        4916,
        "test_module_error_names_module_file",
        "dloc_module_file.TestModuleDiagLocation4916.test_module_error_names_module_file",
    ),
    (
        4916,
        "test_entry_error_names_entry_file",
        "dloc_module_file.TestModuleDiagLocation4916.test_entry_error_names_entry_file",
    ),
    (
        4916,
        "test_deny_after_std_require_is_in_entry_file",
        "dloc_module_file.TestModuleDiagLocation4916.test_deny_after_std_require_is_in_entry_file",
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
