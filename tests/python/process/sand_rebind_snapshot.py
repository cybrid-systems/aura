#!/usr/bin/env python3
"""Regression for #4937: rebind + snapshot/restore under Restricted.

A live workspace define is snapshotted, another define is rebound, then the
snapshot is restored and eval-current runs. Under aura soft / --preset dev the
restore guard used to abort on a production density-streak latch and install
the pre-restore children list onto the smaller restored flat. Lowering then
saw a node id past flat.size() and aborted (rc 134). AURA_SANDBOX=off never
took that deny, so the same program printed (1 2).

The children snapshot from before a wholesale assign_from_image is not a
pre-image of the restored columns. Abort no longer installs it.

Also locks mutate:rebind on a name that is not in the workspace: the call
returns #t and adds a top-level define (the #165 add path). The new define
survives snapshot/restore.
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

# The issue's program. `nope` is not in the workspace; its body calls member.
REPRO = r"""
(set-code "(define (params) (list 1 2))\n")
(mutate :rebind "nope" "(lambda (xs r) (member r xs))" "x")
(define snap (mutate :snapshot "s1"))
(mutate :rebind "params" "(lambda () (list 3 4))" "step")
(mutate :restore snap)
(eval-current)
(display (params))
(newline)
"""

# Add path: missing name returns #t, the define runs, and restore keeps it.
ADD = r"""
(set-code "(define (params) (list 1 2))\n")
(display (list "add" (mutate :rebind "nope" "(lambda (xs r) (member r xs))" "x")))
(newline)
(define snap (mutate :snapshot "s1"))
(mutate :rebind "params" "(lambda () (list 3 4))" "step")
(display (list "restore" (mutate :restore snap)))
(newline)
(eval-current)
(display (list "params" (params)))
(newline)
(display (list "nope" (nope (list 1 2 3) 2)))
(newline)
"""

# Existing name, body calls member. Same snapshot/restore shape.
EXISTING = r"""
(set-code "(define (params) (list 1 2))\n(define (has? xs r) (car xs))\n")
(mutate :rebind "has?" "(lambda (xs r) (member r xs))" "x")
(define snap (mutate :snapshot "s1"))
(mutate :rebind "params" "(lambda () (list 3 4))" "step")
(mutate :restore snap)
(eval-current)
(display (params))
(newline)
(display (has? (list 9 8) 8))
(newline)
"""

# A rejected rebind must still roll the body back. The #4937 size guard only
# skips a children snapshot that is longer than the live flat (wholesale
# replace). A failed parse-grow is the smaller-snapshot case and still restores.
ROLLBACK = r"""
(set-code "(define (params) (list 1 2))\n")
(eval-current)
(display (mutate :rebind "params" "(+ 1 \"x\")" "x"))
(newline)
(eval-current)
(display (params))
(newline)
"""

STRICTS = {"default": None, "strict0": "0", "force-soa": "force-soa"}
SESSIONS = {
    "soft": (["soft"], {}),
    "preset-dev": (["--preset", "dev"], {}),
    "sandbox-off": ([], {"AURA_SANDBOX": "off"}),
}


def _env(strict: str | None, extra: dict[str, str]) -> dict[str, str]:
    env = dict(os.environ)
    for k in ("AURA_PIPELINE_STRICT", "AURA_SANDBOX", "AURA_ARENA_MOVING_COMPACT", "AURA_KEEP_GOING"):
        env.pop(k, None)
    if strict is not None:
        env["AURA_PIPELINE_STRICT"] = strict
    env.update(extra)
    return env


class RebindSnapshotMember(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not AURA_BIN.exists():
            raise unittest.SkipTest(f"aura binary not found: {AURA_BIN}")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        (cls.dir / "repro.aura").write_text(REPRO)
        (cls.dir / "add.aura").write_text(ADD)
        (cls.dir / "existing.aura").write_text(EXISTING)
        (cls.dir / "rollback.aura").write_text(ROLLBACK)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def _run(self, name: str, strict: str | None, args: list[str], extra: dict[str, str]):
        return subprocess.run(
            [str(AURA_BIN), *args, name],
            capture_output=True,
            text=True,
            env=_env(strict, extra),
            cwd=self.dir,
            timeout=120,
            stdin=subprocess.DEVNULL,
        )

    def test_repro_all_modes(self) -> None:
        for sname, strict in STRICTS.items():
            for vname, (args, extra) in SESSIONS.items():
                with self.subTest(strict=sname, session=vname):
                    p = self._run("repro.aura", strict, args, extra)
                    self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                    self.assertNotIn("contract violation", p.stderr)
                    lines = [ln for ln in p.stdout.splitlines() if ln.startswith("(")]
                    self.assertEqual(lines, ["(1 2)"], p.stdout + p.stderr)

    def test_missing_name_adds_and_survives_restore(self) -> None:
        for vname, (args, extra) in SESSIONS.items():
            with self.subTest(session=vname):
                p = self._run("add.aura", None, args, extra)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                text = p.stdout
                self.assertIn("(add #t)", text)
                self.assertIn("(restore #t)", text)
                self.assertIn("(params (1 2))", text)
                self.assertIn("(nope (2 3))", text, p.stderr)

    def test_existing_name_member_body(self) -> None:
        p = self._run("existing.aura", None, ["soft"], {})
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        lines = [ln for ln in p.stdout.splitlines() if ln.startswith("(")]
        self.assertEqual(lines, ["(1 2)", "(8)"], p.stdout + p.stderr)

    def test_failed_rebind_still_rolls_back(self) -> None:
        p = self._run("rollback.aura", None, ["soft"], {})
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("(1 2)", p.stdout)
        self.assertNotIn("(params (list 3 4))", p.stdout)


if __name__ == "__main__":
    unittest.main()
