"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "mutate"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "mutate_no_workspace",
    "mutate_stable_ref",
    "mutate_densify",
    "mutate_cache_ir",
    "mutate_vector_fill",
    "mutate_string_index",
)
CASES = (
    (
        5164,
        "test_numeric_node_without_workspace_is_no_workspace",
        "mutate_no_workspace.TestIssue5164MutateNoWorkspace.test_numeric_node_without_workspace_is_no_workspace",
    ),
    (
        5165,
        "test_zero_args_is_bad_arg",
        "mutate_stable_ref.TestIssue5165CheckStableRefArity.test_zero_args_is_bad_arg",
    ),
    (
        5151,
        "test_define_sugar_repro",
        "mutate_densify.DensifyWedge5151.test_define_sugar_repro",
    ),
    (
        5151,
        "test_lambda_workspace",
        "mutate_densify.DensifyWedge5151.test_lambda_workspace",
    ),
    (
        5151,
        "test_refused_mutate_then_admits",
        "mutate_densify.DensifyWedge5151.test_refused_mutate_then_admits",
    ),
    (
        5159,
        "test_cache_round_trip_reports_ir_and_refuses_a_truncated_section",
        "mutate_cache_ir.CacheIr5159.test_cache_round_trip_reports_ir_and_refuses_a_truncated_section",
    ),
    (
        5159,
        "test_inspect_ir_prints_the_module",
        "mutate_cache_ir.CacheIr5159.test_inspect_ir_prints_the_module",
    ),
    (
        4874,
        "test_restricted_denies_vector_fill",
        "mutate_vector_fill.TestVectorFillMutate4874.test_restricted_denies_vector_fill",
    ),
    (
        4874,
        "test_local_vector_is_not_shared_state",
        "mutate_vector_fill.TestVectorFillMutate4874.test_local_vector_is_not_shared_state",
    ),
    (
        4874,
        "test_off_allows_vector_fill",
        "mutate_vector_fill.TestVectorFillMutate4874.test_off_allows_vector_fill",
    ),
    (
        5166,
        "test_wrong_typed_index_is_catchable",
        "mutate_string_index.TestIssue5166StringIndexType.test_wrong_typed_index_is_catchable",
    ),
    (
        5166,
        "test_uncaught_cli_is_a_type_error",
        "mutate_string_index.TestIssue5166StringIndexType.test_uncaught_cli_is_a_type_error",
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
