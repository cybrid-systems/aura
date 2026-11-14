"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "json-limits"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "json_depth",
    "json_escape",
)
CASES = (
    (
        5152,
        "test_depth_limit_is_catchable",
        "json_depth.JsonDepth5152.test_depth_limit_is_catchable",
    ),
    (
        5152,
        "test_uncaught_repro_survives",
        "json_depth.JsonDepth5152.test_uncaught_repro_survives",
    ),
    (
        5152,
        "test_help_text_names_the_limit",
        "json_depth.JsonDepth5152.test_help_text_names_the_limit",
    ),
    (
        0,
        "test_strict_parser_accepts_each_shape",
        "json_escape.TestJsonEscapeControls.test_strict_parser_accepts_each_shape",
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
