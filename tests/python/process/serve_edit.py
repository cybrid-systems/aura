#!/usr/bin/env python3
"""Regression for #5168: --serve edit loop is not an internal pipeline error.

set-code and eval-current used to hit the production tree-walker gate and
answer with kind 10 before the capability check. The session now uses the
file-run walker face unless AURA_PIPELINE_STRICT pins one, and a refusal is
a user-level diagnostic. AURA_SANDBOX stays unset. The bug row does not set
AURA_PIPELINE_STRICT=0 (that hid the gate).
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

SET_CODE = '(set-code "(display 1)")'
LOOP = SET_CODE + "\n(eval-current)\n"
DENIED = "effect-denied: mutate not granted tenant=0 op=set-code"
INTERNAL = "internal error"
FORBIDDEN = "tree-walker fallback forbidden"
KIND10 = '"kind":10'
WALKER = "this session forbids the tree walker"
DEV_CAPS = "aura: preset=dev sandbox=restricted caps=mutate,read,write (tenant=0, ws=0)"


def serve(args: list[str], text: str, *, strict: str | None = None) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
    if strict is not None:
        env["AURA_PIPELINE_STRICT"] = strict
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    with tempfile.TemporaryDirectory() as tmp:
        return subprocess.run(
            [str(AURA_BIN), *args],
            input=text,
            cwd=tmp,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )


class TestIssue5168ServeEditLoop(unittest.TestCase):
    def test_default_face_is_effect_denied(self) -> None:
        proc = serve(["--serve"], LOOP)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertNotIn(INTERNAL, blob)
        self.assertNotIn(FORBIDDEN, blob)
        self.assertNotIn(KIND10, proc.stdout)
        self.assertIn('"kind":16', proc.stdout)
        self.assertIn(DENIED, proc.stdout)
        self.assertIn('{"status":"ok","value":"()"}', proc.stdout)

    def test_preset_dev_runs_the_loop(self) -> None:
        proc = serve(["--serve", "--preset", "dev"], LOOP)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(DEV_CAPS, proc.stderr)
        self.assertIn('{"status":"ok","value":"#t"}', proc.stdout)
        self.assertIn('1{"status":"ok","value":"()"}', proc.stdout)
        self.assertNotIn(INTERNAL, blob)
        self.assertNotIn(FORBIDDEN, blob)
        self.assertNotIn(KIND10, proc.stdout)
        self.assertNotIn(DENIED, blob)

    def test_plain_display_stays_ok(self) -> None:
        proc = serve(["--serve"], "(display 1)\n")
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn('1{"status":"ok","value":"()"}', proc.stdout)
        self.assertNotIn(INTERNAL, blob)
        self.assertNotIn(KIND10, proc.stdout)

    def test_pinned_strict_is_user_level(self) -> None:
        proc = serve(["--serve"], LOOP, strict="1")
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertNotIn(INTERNAL, blob)
        self.assertNotIn(KIND10, proc.stdout)
        self.assertNotIn('"kind":16', proc.stdout)
        self.assertIn(WALKER, proc.stdout)
        self.assertIn("AURA_PIPELINE_STRICT", proc.stdout)
        self.assertEqual(proc.stdout.count('"kind":17'), 2, proc.stdout)

    def test_help_names_the_edit_loop(self) -> None:
        proc = serve(["help", "--all"], "")
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn("(set-code", blob)
        self.assertIn("(eval-current)", blob)
        self.assertIn("--serve --preset dev", blob)
        self.assertIn("AURA_PIPELINE_STRICT", blob)


if __name__ == "__main__":
    unittest.main()
