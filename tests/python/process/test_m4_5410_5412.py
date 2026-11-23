"""Issue #5410-#5412: M4 live-fork / hot-state / source-fidelity checks."""

from __future__ import annotations

import unittest

PROCESS_THEME = "m4-5410-5412"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 300
PARTS = ("m4_5410", "m4_5411", "m4_5412")
CASES = (
    (5410, "test_snapshot_contract", "m4_5410.Fork.test_snapshot_contract"),
    (5410, "test_overlay_is_the_fork_api", "m4_5410.Fork.test_overlay_is_the_fork_api"),
    (5411, "test_rebind_preserves_state", "m4_5411.HotState.test_rebind_preserves_state"),
    (
        5412,
        "test_untouched_comments_and_layout_survive",
        "m4_5412.SourceFidelity.test_untouched_comments_and_layout_survive",
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
