#!/usr/bin/env python3
"""Regression for #5165: mutate:check-stable-ref must not crash with no args.

Under --preset dev, (mutate:check-stable-ref) used to read a[0] on an
empty span and abort. It now returns the same catchable bad-arg as a
non-pair argument. AURA_SANDBOX=off is left unset so the mutate grant
comes from --preset dev.
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

USAGE = "usage: (mutate :check-stable-ref (id . gen))"

PROG = r"""
(define (show tag thunk)
  (let ((r (try (thunk) (catch e e))))
    (display tag)
    (display " ")
    (display (if (pair? r) (car r) r))
    (display " ")
    (display (if (and (pair? r) (pair? (cdr r))) (car (cdr r)) ""))
    (newline)))
(show "ZERO" (lambda () (mutate:check-stable-ref)))
(show "ONE" (lambda () (mutate:check-stable-ref 0)))
"""


class TestIssue5165CheckStableRefArity(unittest.TestCase):
    def test_zero_args_is_bad_arg(self) -> None:
        env = os.environ.copy()
        for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
            env.pop(key, None)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p.aura"
            path.write_text(PROG, encoding="utf-8")
            proc = subprocess.run(
                [str(AURA_BIN), "--preset", "dev", "--quiet", str(path)],
                cwd=tmp,
                env=env,
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
        out = proc.stdout
        err = proc.stderr
        self.assertEqual(proc.returncode, 0, f"rc={proc.returncode}\nstdout={out!r}\nstderr={err!r}")
        self.assertNotIn("AURA CRASH", err)
        self.assertNotIn("SIGSEGV", err)
        self.assertNotIn("SIGABRT", err)
        self.assertIn(f"ZERO bad-arg {USAGE}", out)
        self.assertIn(f"ONE bad-arg {USAGE}", out)


if __name__ == "__main__":
    unittest.main()
