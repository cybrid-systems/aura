#!/usr/bin/env python3
"""Issue #1432 freeze + #1448 SlimSurface --strict — unit + synthetic injection.

Run:  python3 tests/test_primitive_surface_gate.py
Also invoked from ./build.py gate via cmd_primitive_surface.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "check_primitive_surface.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("check_primitive_surface", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestBlockedPatterns(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load_module()

    def test_stats_blocked(self):
        self.assertEqual(self.m.blocked_category("query:foo-stats"), "stats")
        self.assertEqual(self.m.blocked_category("compile:bar-stats-hash"), "stats")
        self.assertTrue(self.m.is_blocked("query:macro-hygiene-stats"))

    def test_convenience_blocked(self):
        cases = [
            ("string-upcase", "string"),
            ("string:split", "string"),
            ("json-stringify", "json"),
            ("json:parse", "json"),
            ("math-clamp", "math"),
            ("math:sin", "math"),
            ("vector-map", "vector"),
            ("vector:push", "vector"),
            ("path-join", "path"),
            ("path:resolve", "path"),
            ("time-now", "time"),
            ("time:sleep", "time"),
            ("ast:ref-get-extra", "ast:ref"),
        ]
        for name, cat in cases:
            with self.subTest(name=name):
                self.assertEqual(self.m.blocked_category(name), cat)

    def test_s0_core_not_blocked(self):
        for name in (
            "cons",
            "+",
            "query:root",
            "mutate:rebind",
            "engine:metrics",
            "stats:list",
            "set-code",
            "eval-current",
        ):
            with self.subTest(name=name):
                self.assertIsNone(self.m.blocked_category(name))
                self.assertFalse(self.m.is_blocked(name))

    def test_allowlist_empty(self):
        self.assertEqual(len(self.m.ALLOWLIST), 0)

    def test_freeze_violations_detects_new(self):
        base = ["string-append", "json-parse", "query:foo-stats"]
        cur = base + ["string-brand-new-zzz", "math-fake-zzz"]
        viol = self.m.freeze_violations(cur, base)
        self.assertEqual(viol, ["math-fake-zzz", "string-brand-new-zzz"])

    def test_freeze_violations_allows_removal(self):
        base = ["string-append", "json-parse"]
        cur = ["string-append"]
        self.assertEqual(self.m.freeze_violations(cur, base), [])

    # ── Issue #1448 SlimSurface constants ──
    def test_slim_surface_budgets(self):
        self.assertEqual(self.m.TARGET_BUDGET, 420)
        self.assertGreaterEqual(self.m.INTERIM_HARD_CEILING, self.m.TARGET_BUDGET)
        self.assertEqual(self.m.INTERIM_HARD_CEILING, 700)


class TestSyntheticInjection(unittest.TestCase):
    """Simulate a deliberately-bad add() by monkeypatching scan results."""

    @classmethod
    def setUpClass(cls):
        cls.m = _load_module()

    def test_injected_string_name_fails_check_logic(self):
        real_names = self.m.scan_registered_names()
        frozen = self.m.collect_frozen_names(real_names)
        # Grandfathered set must not include the fake name
        fake = "string-deliberately-bad-1432"
        self.assertNotIn(fake, frozen)
        self.assertTrue(self.m.is_blocked(fake))
        # Baseline from disk
        import json

        baseline = json.loads(self.m.BASELINE_PATH.read_text(encoding="utf-8"))
        base_names = baseline.get("names", [])
        injected = frozen + [fake]
        viol = self.m.freeze_violations(injected, base_names)
        self.assertIn(fake, viol)

    def test_live_gate_passes(self):
        r = subprocess.run(
            [sys.executable, str(SCRIPT)],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(r.returncode, 0, msg=r.stderr + r.stdout)
        self.assertIn("OK: primitive surface freeze", r.stdout)

    def test_live_strict_passes(self):
        """Issue #1448: --strict must pass on the current tree."""
        r = subprocess.run(
            [sys.executable, str(SCRIPT), "--strict"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(r.returncode, 0, msg=r.stderr + r.stdout)
        self.assertIn("SlimSurface --strict", r.stdout)
        self.assertIn("OK: SlimSurface --strict checks passed", r.stdout)

    def test_strict_fails_when_over_ceiling(self):
        """Budget hard-fail when public count exceeds INTERIM_HARD_CEILING."""
        m = self.m
        # Fabricate a huge name list above the ceiling.
        fake = [f"core-fake-{i}" for i in range(m.INTERIM_HARD_CEILING + 1)]
        rc = m.run_strict_checks(fake, stats_names=[])
        self.assertEqual(rc, 1)


def main() -> int:
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
