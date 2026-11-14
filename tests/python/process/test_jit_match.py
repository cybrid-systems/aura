"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "jit-match"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "jit_recursion",
    "jit_tail",
    "jit_let",
    "jit_compound",
)
CASES = (
    (
        5173,
        "test_recursive_shapes_match_the_interpreter",
        "jit_recursion.TestIssue5173JitRecursion.test_recursive_shapes_match_the_interpreter",
    ),
    (
        5173,
        "test_l1_off_still_matches",
        "jit_recursion.TestIssue5173JitRecursion.test_l1_off_still_matches",
    ),
    (
        5174,
        "test_named_let_100000_matches_interpreter",
        "jit_tail.TestIssue5174JitTailLoop.test_named_let_100000_matches_interpreter",
    ),
    (
        5174,
        "test_deep_nontail_refuses",
        "jit_tail.TestIssue5174JitTailLoop.test_deep_nontail_refuses",
    ),
    (
        5174,
        "test_deep_mutual_refuses",
        "jit_tail.TestIssue5174JitTailLoop.test_deep_mutual_refuses",
    ),
    (
        5175,
        "test_jit_values_match_interpreter",
        "jit_let.TestIssue5175JitLetBinding.test_jit_values_match_interpreter",
    ),
    (
        5176,
        "test_jit_display_matches_interpreter",
        "jit_compound.TestIssue5176JitCompoundDisplay.test_jit_display_matches_interpreter",
    ),
    (
        5176,
        "test_write_file_matches_interpreter",
        "jit_compound.TestIssue5176JitCompoundDisplay.test_write_file_matches_interpreter",
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
