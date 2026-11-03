#!/usr/bin/env python3
"""Regression for #4896: Restricted mutate deny must not be silent on force-soa.

With AURA_SANDBOX unset (production Restricted) a top-level `set-car!` /
`vector-set!` is refused by the dispatch Mutate gate, which returns an
`effect-denied` error VALUE. The force-soa per-form face (and the IR in
statement position) dropped that value: the write was lost, nothing was
printed and the run exited 0. It must report effect-denied, rc != 0.
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


def _run(source: str, direct: bool, strict: str, sandbox: str | None = None):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_SANDBOX", None)
    if sandbox is not None:
        env["AURA_SANDBOX"] = sandbox
    env["AURA_PIPELINE_STRICT"] = strict
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


SETCAR = "(define q (cons 1 2))\n(set-car! q 5)\n(display (list 'q q)) (newline)\n"
VSET = "(define v (vector 1 2))\n(vector-set! v 0 9)\n(display (list 'v v)) (newline)\n"
TRY = '(define v (vector 1 2))\n(display (try (vector-set! v 0 9) (catch (e) (list \'caught (format "~a" e)))))\n'


class TestRestrictedMutateDenyForceSoa4896(unittest.TestCase):
    def test_toplevel_mutate_deny_reported(self) -> None:
        for src, op in ((SETCAR, "set-car!"), (VSET, "vector-set!")):
            for direct in (True, False):
                for strict in ("force-soa", "0"):
                    p = _run(src, direct, strict)
                    tag = f"op={op} direct={direct} strict={strict}"
                    self.assertNotEqual(p.returncode, 0, f"{tag}: {p.stdout} {p.stderr}")
                    self.assertIn(f"effect-denied: mutate not granted tenant=0 op={op}", p.stderr, tag)

    def test_deny_still_catchable(self) -> None:
        for strict in ("force-soa", "0"):
            p = _run(TRY, True, strict)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("(caught effect-denied: mutate not granted", p.stdout, strict)

    def test_sandbox_off_still_mutates(self) -> None:
        for strict in ("force-soa", "0"):
            p = _run(SETCAR + VSET, True, strict, sandbox="off")
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.split(), ["(q", "(5", ".", "2))", "(v", "#(9", "2))"])


if __name__ == "__main__":
    unittest.main()
