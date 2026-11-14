#!/usr/bin/env python3
"""Regression for #4909 / #4908: a failure inside an IR-lowered procedure keeps its cause.

Before: under AURA_PIPELINE_STRICT=force-soa, `aura --load f` in the Restricted
default ran a set-car! inside a procedure, the sandbox denied it, and the IR
define bridge dropped that diagnostic. The user saw
"error: invalid closure: eval_flat: invalid closure" with no location or hint,
while every other face reported "effect-denied: mutate not granted".
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

PROG = (
    "(define (bump! r) (let ((v (+ (car r) 1))) (set-car! r v) v))\n"
    "(define r (list 1))\n"
    "(display (bump! r)) (newline)\n"
)


class IrBridgeDiag(unittest.TestCase):
    def run_aura(self, *args: str, strict: str | None) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
            env.pop(k, None)
        if strict:
            env["AURA_PIPELINE_STRICT"] = strict
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(PROG)
            return subprocess.run(
                [str(AURA_BIN), *args, "p.aura"],
                cwd=d,
                capture_output=True,
                text=True,
                env=env,
                timeout=120,
            )

    def test_deny_keeps_its_cause_on_every_face(self) -> None:
        for strict in (None, "0", "force-soa"):
            for face in ((), ("--load",)):
                with self.subTest(strict=strict, face=face):
                    r = self.run_aura(*face, strict=strict)
                    self.assertNotEqual(r.returncode, 0)
                    self.assertIn("mutate not granted", r.stderr)
                    self.assertNotIn("invalid closure", r.stderr)


if __name__ == "__main__":
    unittest.main()
