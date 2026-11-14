"""Process checks for one face.

The case table is the runner contract. Issue numbers stay in the part
files, so this banner names no issue.
"""

from __future__ import annotations

import unittest

PROCESS_THEME = "offload"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = (
    "offload_metrics",
    "offload_primitives",
)
CASES = (
    (
        4884,
        "test_metrics_faces_expose_offload",
        "offload_metrics.TestOffloadMetrics.test_metrics_faces_expose_offload",
    ),
    (
        0,
        "test_offload_sync_in_plain_aura",
        "offload_primitives.TestOffloadPrimitives.test_offload_sync_in_plain_aura",
    ),
    (
        0,
        "test_offload_async_in_serve_async",
        "offload_primitives.TestOffloadPrimitives.test_offload_async_in_serve_async",
    ),
    (
        0,
        "test_eval_async_isolated_result",
        "offload_primitives.TestOffloadPrimitives.test_eval_async_isolated_result",
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
