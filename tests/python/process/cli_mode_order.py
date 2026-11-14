#!/usr/bin/env python3
"""Regression for #5170: tool modes need not be the first argument.

`aura --preset dev --serve` used to die as `unknown option '--serve'`.
`aura --inspect --serve` used to run --inspect and drop --serve.
One of --serve / --inspect / --typecheck / --ir / --query may follow
options. Two modes name the conflict. AURA_SANDBOX stays unset.
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

JSON3 = '{"status":"ok","value":"3"}'
DEV_CAPS = "aura: preset=dev sandbox=restricted caps=mutate,read,write (tenant=0, ws=0)"
GRANT_CAPS = "aura: preset=custom sandbox=restricted caps=mutate,read,write (tenant=0, ws=0)"
DENY_CAPS = "aura: preset=dev sandbox=restricted caps=mutate,read (tenant=0, ws=0)"
UNKNOWN = "unknown option"
EXPR = "(+ 1 2)\n"


def run_aura(args: list[str], text: str = EXPR) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
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


class TestIssue5170ModeOrder(unittest.TestCase):
    def test_preset_dev_before_serve_grants_write(self) -> None:
        proc = run_aura(["--preset", "dev", "--serve"])
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(JSON3, proc.stdout)
        self.assertIn(DEV_CAPS, proc.stderr)
        self.assertNotIn(UNKNOWN, blob)

    def test_grant_write_before_serve(self) -> None:
        proc = run_aura(["--grant", "write", "--serve"])
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(JSON3, proc.stdout)
        self.assertIn(GRANT_CAPS, proc.stderr)
        self.assertNotIn(UNKNOWN, blob)

    def test_quiet_before_serve_drops_the_banner(self) -> None:
        plain = run_aura(["--quiet", "--serve"])
        blob = plain.stdout + plain.stderr
        self.assertEqual(plain.returncode, 0, blob)
        self.assertIn(JSON3, plain.stdout)
        self.assertNotIn(UNKNOWN, blob)
        self.assertNotIn("caps=", plain.stderr)

        quiet = run_aura(["--preset", "dev", "--quiet", "--serve"])
        blob = quiet.stdout + quiet.stderr
        self.assertEqual(quiet.returncode, 0, blob)
        self.assertIn(JSON3, quiet.stdout)
        self.assertNotIn("caps=", quiet.stderr)
        self.assertNotIn(UNKNOWN, blob)

    def test_deny_write_before_serve(self) -> None:
        bare = run_aura(["--deny", "write", "--serve"])
        blob = bare.stdout + bare.stderr
        self.assertEqual(bare.returncode, 0, blob)
        self.assertIn(JSON3, bare.stdout)
        self.assertNotIn(UNKNOWN, blob)

        denied = run_aura(["--preset", "dev", "--deny", "write", "--serve"])
        blob = denied.stdout + denied.stderr
        self.assertEqual(denied.returncode, 0, blob)
        self.assertIn(JSON3, denied.stdout)
        self.assertIn(DENY_CAPS, denied.stderr)
        self.assertNotIn("caps=mutate,read,write", denied.stderr)

    def test_serve_before_preset_still_works(self) -> None:
        proc = run_aura(["--serve", "--preset", "dev"])
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(JSON3, proc.stdout)
        self.assertIn(DEV_CAPS, proc.stderr)

    def test_two_modes_conflict(self) -> None:
        rows = (
            ["--inspect", "--serve"],
            ["--typecheck", "--serve"],
            ["--serve", "--inspect"],
        )
        for args in rows:
            proc = run_aura(args)
            blob = proc.stdout + proc.stderr
            self.assertNotEqual(proc.returncode, 0, blob)
            self.assertIn("conflicting modes", proc.stderr)
            self.assertIn(args[0], proc.stderr)
            self.assertIn(args[1], proc.stderr)
            self.assertNotIn(UNKNOWN, blob)
            self.assertNotIn(JSON3, proc.stdout)
            self.assertEqual(proc.stdout, "")

    def test_other_tool_modes_follow_options(self) -> None:
        typed = run_aura(["--quiet", "--typecheck"])
        blob = typed.stdout + typed.stderr
        self.assertEqual(typed.returncode, 0, blob)
        self.assertIn("no errors", typed.stdout)
        self.assertNotIn(UNKNOWN, blob)

        ir = run_aura(["--preset", "dev", "--ir"])
        blob = ir.stdout + ir.stderr
        self.assertEqual(ir.returncode, 0, blob)
        self.assertEqual(ir.stdout.strip(), "3")
        self.assertNotIn(UNKNOWN, blob)
        self.assertNotIn("caps=", ir.stderr)

        queried = run_aura(["--quiet", "--query", ":calls"])
        blob = queried.stdout + queried.stderr
        self.assertEqual(queried.returncode, 0, blob)
        self.assertIn("query:", queried.stdout)
        self.assertNotIn(UNKNOWN, blob)

    def test_help_names_both_orders(self) -> None:
        proc = run_aura(["help", "--all"], text="")
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn("--serve --preset dev", blob)
        self.assertIn("--preset dev --serve", blob)

    def test_oneshot_without_a_tool_mode(self) -> None:
        proc = run_aura(["--preset", "dev"])
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertEqual(proc.stdout.strip(), "3")
        self.assertIn(DEV_CAPS, proc.stderr)
        self.assertNotIn(UNKNOWN, blob)

    def test_load_after_preset_stays_oneshot(self) -> None:
        env = os.environ.copy()
        for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
            env.pop(key, None)
        env["AURA_RUNTIME_DIR"] = str(ROOT)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p.aura"
            path.write_text("(display 1)\n", encoding="utf-8")
            proc = subprocess.run(
                [str(AURA_BIN), "--preset", "dev", "--load", str(path)],
                cwd=tmp,
                env=env,
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn("1", proc.stdout)
        self.assertIn(DEV_CAPS, proc.stderr)
        self.assertNotIn(UNKNOWN, blob)

    def test_worker_threads_before_serve(self) -> None:
        proc = run_aura(["--worker-threads", "1", "--serve"])
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(JSON3, proc.stdout)
        self.assertNotIn(UNKNOWN, blob)


if __name__ == "__main__":
    unittest.main()
