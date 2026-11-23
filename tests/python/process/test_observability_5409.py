"""Issue #5409: external system-tool observability.

The case table is the runner contract; each name lives in obs_5409.py so the
runner can validate coverage without importing the module.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "obs-5409"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ("obs_5409",)
CASES = (
    (5409, "test_jit_names_are_in_the_perf_map", "obs_5409.PerfMap.test_jit_names_are_in_the_perf_map"),
    (5409, "test_perf_map_is_opt_in", "obs_5409.PerfMap.test_perf_map_is_opt_in"),
    (5409, "test_probe_notes_are_in_the_binary", "obs_5409.Usdt.test_probe_notes_are_in_the_binary"),
    (5409, "test_runtime_symbols_are_exported", "obs_5409.Usdt.test_runtime_symbols_are_exported"),
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
