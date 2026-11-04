#!/usr/bin/env python3
"""Regression for #4899: top-level statement-position runtime errors are not silent.

A runtime error in statement position (the value is not used) was dropped on
the IR faces: nothing was printed and the run exited 0. The tree-walker face
already raised it. The IR entry now lets an error value escape a non-tail
top-level statement, and file mode turns the escaped value into a located
diagnostic (resuming the remaining forms, the #4460 contract).
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def _run(source: str, direct: bool, strict: str):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    if strict:
        env["AURA_PIPELINE_STRICT"] = strict
    else:
        env.pop("AURA_PIPELINE_STRICT", None)
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


VREF = '(define v (vector 1 2))\n(vector-ref v 5)\n(display "after") (newline)\n'

ST_CAR = '(define x 1)\n(car 5)\n(display "after") (newline)\n'
ST_ERR = '(define x 1)\n(error "boom")\n(display "after") (newline)\n'
ST_NESTED = '(display "x")\n(vector-ref (vector 1 2) 5)\n(display "after") (newline)\n'


class TestStatementPositionErrors4899(unittest.TestCase):
    def _assert_reported(self, src: str, needle: str) -> None:
        for strict in ("", "0", "force-soa"):
            for direct in (True, False):
                p = _run(src, direct=direct, strict=strict)
                tag = f"strict={strict or 'default'} direct={direct}"
                self.assertNotEqual(p.returncode, 0, f"{tag}: {p.stdout!r} {p.stderr!r}")
                self.assertIn(needle, p.stderr, f"{tag}: {p.stderr!r}")
                # The direct file face resumes after the failing form
                # (#4460); --load stops at the first failed form, which is its
                # existing convention.
                if direct:
                    self.assertIn("after", p.stdout, f"{tag}: later forms keep running: {p.stdout!r}")

    def test_vector_ref_statement_position(self) -> None:
        self._assert_reported(VREF, "vector-ref")

    def test_car_statement_position(self) -> None:
        self._assert_reported(ST_CAR, "car")

    def test_error_statement_position(self) -> None:
        self._assert_reported(ST_ERR, "boom")

    def test_nested_vector_ref_statement_position(self) -> None:
        self._assert_reported(ST_NESTED, "vector-ref")

    def test_value_position_still_reports(self) -> None:
        src = "(define v (vector 1 2))\n(display (vector-ref v 5)) (newline)\n"
        for strict in ("", "0", "force-soa"):
            p = _run(src, direct=True, strict=strict)
            self.assertNotEqual(p.returncode, 0, f"{strict}: {p.stdout}")
            self.assertIn("vector-ref", p.stderr, strict)

    def test_try_catch_still_handles_statement_errors(self) -> None:
        src = "(display (try (begin (car 5) (quote no)) (catch (e) (quote caught))))\n(newline)\n"
        for strict in ("", "0", "force-soa"):
            p = _run(src, direct=True, strict=strict)
            self.assertEqual(p.returncode, 0, f"{strict}: {p.stderr}")
            self.assertIn("caught", p.stdout, strict)


if __name__ == "__main__":
    unittest.main()
