#!/usr/bin/env python3
"""Regression for #5167: --serve honours --preset, --grant, and --deny.

Serve used to return before oneshot capability parsing, so the session
stayed Restricted and set-code was effect-denied even with --preset dev.
The same flags now mint that soft session and print its caps line.
AURA_SANDBOX stays unset. AURA_PIPELINE_STRICT=0 is the issue's repro:
the companion pipeline gate answers first when it is left at the default.
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

DEV_CAPS = "aura: preset=dev sandbox=restricted caps=mutate,read,write (tenant=0, ws=0)"
GRANT_CAPS = "aura: preset=custom sandbox=restricted caps=mutate,read (tenant=0, ws=0)"
DEV_NO_WRITE = "aura: preset=dev sandbox=restricted caps=mutate,read (tenant=0, ws=0)"
DEV_DENY_MUTATE = "aura: preset=dev sandbox=restricted caps=read (default)"
SET_CODE = '(set-code "(display 1)")'
DENIED = "effect-denied: mutate not granted tenant=0 op=set-code"


def serve(args: list[str], line: str) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING"):
        env.pop(key, None)
    env["AURA_PIPELINE_STRICT"] = "0"
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    with tempfile.TemporaryDirectory() as tmp:
        return subprocess.run(
            [str(AURA_BIN), *args],
            input=line + "\n",
            cwd=tmp,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )


class TestIssue5167ServePreset(unittest.TestCase):
    def test_preset_dev_grants_set_code(self) -> None:
        proc = serve(["--serve", "--preset", "dev"], SET_CODE)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(DEV_CAPS, proc.stderr)
        self.assertIn('{"status":"ok","value":"#t"}', proc.stdout)
        self.assertNotIn(DENIED, blob)
        self.assertNotIn("AURA CRASH", blob)

    def test_preset_dev_display_repro(self) -> None:
        proc = serve(["--serve", "--preset", "dev"], f"(display {SET_CODE})")
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(DEV_CAPS, proc.stderr)
        self.assertIn("#t", proc.stdout)
        self.assertNotIn(DENIED, blob)
        self.assertNotIn("AURA CRASH", blob)

    def test_grant_mutate(self) -> None:
        proc = serve(["--serve", "--grant", "mutate"], SET_CODE)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(GRANT_CAPS, proc.stderr)
        self.assertIn('{"status":"ok","value":"#t"}', proc.stdout)
        self.assertNotIn(DENIED, blob)

    def test_deny_write_keeps_mutate(self) -> None:
        proc = serve(["--serve", "--preset", "dev", "--deny", "write"], SET_CODE)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(DEV_NO_WRITE, proc.stderr)
        self.assertNotIn("write", proc.stderr)
        self.assertIn('{"status":"ok","value":"#t"}', proc.stdout)
        self.assertNotIn(DENIED, blob)

    def test_deny_mutate_still_refuses(self) -> None:
        proc = serve(["--serve", "--preset", "dev", "--deny", "mutate"], f"(display {SET_CODE})")
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(DEV_DENY_MUTATE, proc.stderr)
        self.assertIn(DENIED, proc.stderr)
        self.assertNotIn("#t", proc.stdout)

    def test_plain_serve_stays_restricted(self) -> None:
        proc = serve(["--serve"], f"(display {SET_CODE})")
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(DENIED, proc.stderr)
        self.assertNotIn("caps=mutate", proc.stderr)
        self.assertNotIn("AURA CRASH", blob)


if __name__ == "__main__":
    unittest.main()
