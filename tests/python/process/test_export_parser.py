"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "export-parser"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "export_contracts",
    "export_unclosed",
)
CASES = (
    (
        5198,
        "test_iota_list_then_iter",
        "export_contracts.TestExportContracts5198.test_iota_list_then_iter",
    ),
    (
        5198,
        "test_iota_iter_then_list",
        "export_contracts.TestExportContracts5198.test_iota_iter_then_list",
    ),
    (
        5198,
        "test_sorted_both_orders",
        "export_contracts.TestExportContracts5198.test_sorted_both_orders",
    ),
    (
        5198,
        "test_vector_map_both_orders",
        "export_contracts.TestExportContracts5198.test_vector_map_both_orders",
    ),
    (
        5198,
        "test_extract_code_both_orders",
        "export_contracts.TestExportContracts5198.test_extract_code_both_orders",
    ),
    (
        5198,
        "test_require_overwrite_warns_and_still_loads",
        "export_contracts.TestExportContracts5198.test_require_overwrite_warns_and_still_loads",
    ),
    (
        5198,
        "test_require_same_module_twice_does_not_warn",
        "export_contracts.TestExportContracts5198.test_require_same_module_twice_does_not_warn",
    ),
    (
        5199,
        "test_paren_semi_file_names_line_1",
        "export_unclosed.TestUnclosedList5199.test_paren_semi_file_names_line_1",
    ),
    (
        5199,
        "test_paren_semi_load_names_line_1",
        "export_unclosed.TestUnclosedList5199.test_paren_semi_load_names_line_1",
    ),
    (
        5199,
        "test_closed_paren_semi_runs",
        "export_unclosed.TestUnclosedList5199.test_closed_paren_semi_runs",
    ),
    (
        5199,
        "test_plain_semicolon_header_runs",
        "export_unclosed.TestUnclosedList5199.test_plain_semicolon_header_runs",
    ),
    (
        5199,
        "test_mid_file_opener_is_not_eof",
        "export_unclosed.TestUnclosedList5199.test_mid_file_opener_is_not_eof",
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
