#!/usr/bin/env python3
"""Regression for #4937.

Restricted soft session (`aura soft` / `--preset dev`): mutate:rebind of a
define whose body calls a general primitive (member, equal?, not), then
ast:snapshot, rebind of another define, ast:restore, eval-current aborted
in lowering (id >= flat.size(), SIGABRT / rc 134). AURA_SANDBOX=off printed
(1 2). The restore boundary's enter checkpoint still held the pre-replace
children_; a CastOp density reject on the way out spliced those ids onto
the image's columns.

Also pins the missing-name contract: mutate:rebind of a name that is not in
the live workspace adds the define and returns #t.
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

PROG = r"""
(set-code "(define (params) (list 1 2))\n")
(mutate :rebind "nope" "(lambda (xs r) (member r xs))" "x")
(define snap (mutate :snapshot "s1"))
(mutate :rebind "params" "(lambda () (list 3 4))" "step")
(mutate :restore snap)
(eval-current)
(display (params))
(newline)
"""

PROG_ADD = r"""
(set-code "(define (f x) (+ x 1))")
(display (mutate :rebind "nonexistent" "(lambda (y) y)" "add"))
(newline)
(eval-current)
(display (nonexistent 7))
(newline)
"""

SESSIONS = {
    "soft": (["soft"], {}),
    "preset-dev": (["--preset", "dev"], {}),
    "sandbox-off": ([], {"AURA_SANDBOX": "off"}),
}


def _env(extra: dict[str, str]) -> dict[str, str]:
    env = dict(os.environ)
    for k in ("AURA_PIPELINE_STRICT", "AURA_SANDBOX", "AURA_ARENA_MOVING_COMPACT"):
        env.pop(k, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    env.update(extra)
    return env


class RebindRestore4937(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not AURA_BIN.exists():
            raise unittest.SkipTest(f"aura binary not found: {AURA_BIN}")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        (cls.dir / "repro.aura").write_text(PROG)
        (cls.dir / "add.aura").write_text(PROG_ADD)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_restore_matrix(self) -> None:
        for name, (args, extra) in SESSIONS.items():
            with self.subTest(session=name):
                p = subprocess.run(
                    [str(AURA_BIN), *args, "repro.aura"],
                    capture_output=True,
                    text=True,
                    env=_env(extra),
                    cwd=self.dir,
                    timeout=60,
                    stdin=subprocess.DEVNULL,
                )
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertIn("(1 2)", p.stdout, p.stdout + p.stderr)
                self.assertNotIn("contract violation", p.stderr)

    def test_rebind_missing_name_adds(self) -> None:
        for name, (args, extra) in SESSIONS.items():
            with self.subTest(session=name):
                p = subprocess.run(
                    [str(AURA_BIN), *args, "add.aura"],
                    capture_output=True,
                    text=True,
                    env=_env(extra),
                    cwd=self.dir,
                    timeout=60,
                    stdin=subprocess.DEVNULL,
                )
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                lines = [ln.strip() for ln in p.stdout.splitlines() if ln.strip()]
                self.assertEqual(lines, ["#t", "7"], p.stdout + p.stderr)


if __name__ == "__main__":
    unittest.main()
