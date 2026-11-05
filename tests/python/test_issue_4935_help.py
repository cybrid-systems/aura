#!/usr/bin/env python3
"""Regression for #4935: CLI --help DX.

Before: `aura new --help` created a directory named `--help`, subcommands had
no --help, an unknown command was "cannot open program file", usage printed
the bundled libexec path, and `aura --help` was ~80 lines on stderr.
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

SUBS = ("new", "run", "test", "repl", "evo", "lsp", "soft", "help")


def run(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        env.pop(k, None)
    return subprocess.run(
        [str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=60, stdin=subprocess.DEVNULL
    )


class Help(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_new_help_creates_nothing(self) -> None:
        for flag in ("--help", "-h"):
            r = run(["new", flag], self.d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("usage: aura new", r.stdout)
        r = run(["new", "app", "--help"], self.d)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run(["new", "--bogus"], self.d)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(list(self.d.iterdir()), [])

    def test_every_subcommand_has_help(self) -> None:
        for sub in SUBS:
            for args in ([sub, "--help"], ["help", sub]):
                r = run(args, self.d)
                self.assertEqual(r.returncode, 0, f"{args}: {r.stderr}")
                self.assertIn(f"usage: aura {sub}", r.stdout, args)
        self.assertEqual(list(self.d.iterdir()), [])

    def test_unknown_command(self) -> None:
        r = run(["nwe"], self.d)
        self.assertEqual(r.returncode, 1)
        self.assertIn("unknown command 'nwe'", r.stderr)
        self.assertIn("did you mean 'aura new'", r.stderr)
        self.assertNotIn("cannot open program file", r.stderr)
        r = run(["help", "nope"], self.d)
        self.assertEqual(r.returncode, 1)
        self.assertIn("unknown command 'nope'", r.stderr)
        # an existing bare-word file still runs
        (self.d / "prog").write_text('(display "hi")')
        r = run(["prog"], self.d)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("hi", r.stdout)

    def test_top_help_concise_no_path(self) -> None:
        r = run(["--help"], self.d)
        self.assertEqual(r.returncode, 0)
        lines = r.stdout.splitlines()
        self.assertLessEqual(len(lines), 25, r.stdout)
        for sub in SUBS:
            self.assertIn(sub, r.stdout)
        self.assertEqual(run(["help"], self.d).stdout, r.stdout)
        self.assertEqual(run(["-h"], self.d).stdout, r.stdout)
        full = run(["help", "--all"], self.d)
        exe = str(Path(AURA_BIN).resolve())
        for out in (r.stdout, full.stdout, run(["nwe"], self.d).stderr, run(["--bogus"], self.d).stderr):
            self.assertNotIn(exe, out)
            self.assertNotIn("libexec", out)
        self.assertIn("usage: aura ", full.stdout)


if __name__ == "__main__":
    unittest.main()
