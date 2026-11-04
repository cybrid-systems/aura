#!/usr/bin/env python3
"""Regression for #4913: aura new / run / test, a complete --help, soft --load.

Before: `aura new foo` was "cannot open program file 'new'", there was no
`aura run` / `aura test`, --help did not list --fmt/--typecheck/--query/…,
and `aura soft --load f` was "unknown option '--load'".
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

EVO = '(set-code "(define p 1)")\n(eval-current)\n(display (mutate:rebind "p" "2" "x"))\n(newline)\n'


def run(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        env.pop(k, None)
    return subprocess.run([str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120)


class FrontDoor(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def new(self, *args: str) -> Path:
        r = run(["new", *args], self.d)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return self.d / args[-1]

    def test_app(self) -> None:
        p = self.new("app", "demo")
        for f in (
            "aura.toml",
            "main.aura",
            "lib/greet.aura",
            "tests/greet_test.aura",
            "tests/greet_test.golden",
            ".github/workflows/ci.yml",
            "README.md",
        ):
            self.assertTrue((p / f).is_file(), f)
        self.assertIn("# demo", (p / "README.md").read_text())
        r = run(["run"], p)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("hello, world", r.stdout)
        self.assertIn("entry=main.aura", r.stderr)
        r = run(["run", "--quiet"], p / "lib")  # from a subdirectory
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, "")
        r = run(["test"], p)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("1 passed, 0 failed", r.stdout)

    def test_evolve_report_under_5s(self) -> None:
        p = self.new("evolve", "demo")
        t0 = time.monotonic()
        r = run(["run"], p)
        self.assertLess(time.monotonic() - t0, 5.0)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("preset=dev", r.stderr)
        self.assertIn("KEEP", r.stdout)
        self.assertIn("<table>", (p / "out" / "report.html").read_text())
        self.assertTrue((p / "out" / "audit.tsv").is_file())
        self.assertEqual(run(["test"], p).returncode, 0)

    def test_agent(self) -> None:
        p = self.new("agent", "demo")
        r = run(["run"], p)
        self.assertIn("stages ok 2, failed 0", r.stdout, r.stderr)
        self.assertEqual(run(["test"], p).returncode, 0)

    def test_default_template_and_errors(self) -> None:
        p = self.new("plain")
        self.assertTrue((p / "lib" / "greet.aura").is_file())
        r = run(["new", "app", "plain"], self.d)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not empty", r.stderr)
        r = run(["new", "nope", "x"], self.d)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown template", r.stderr)
        r = run(["run"], self.d)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no aura.toml", r.stderr)
        self.assertIn("new app", r.stderr)

    def test_golden_diff_and_update(self) -> None:
        p = self.new("app", "demo")
        (p / "tests" / "greet_test.golden").write_text("hello, aura\n11\n")
        r = run(["test"], p)
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL tests/greet_test.aura", r.stdout)
        self.assertIn("line 2", r.stdout)
        self.assertEqual(run(["test", "--update"], p).returncode, 0)
        self.assertEqual(run(["test"], p).returncode, 0)

    def test_help_lists_commands(self) -> None:
        r = run(["--help"], self.d)
        for w in (
            "new",
            "run",
            "test",
            "--fmt",
            "--check",
            "--typecheck",
            "--evo-explain",
            "--query",
            "--inspect",
            "--load",
            "--preset",
        ):
            self.assertIn(w, r.stderr, w)
        self.assertEqual(run(["help"], self.d).stderr, r.stderr)

    def test_soft_load(self) -> None:
        (self.d / "evo.aura").write_text(EVO)
        for args in (["soft", "--load", "evo.aura"], ["--preset", "dev", "--load", "evo.aura"]):
            r = run(args, self.d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("#t", r.stdout)

    def test_embedded_templates_current(self) -> None:
        out = self.d / "t.inc"
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts/tools/gen_cli_templates_4913.py"),
                str(ROOT / "templates/new"),
                str(out),
            ],
            check=True,
            capture_output=True,
        )
        self.assertEqual(
            out.read_text(),
            (ROOT / "src/cli_templates_4913.inc").read_text(),
            "run scripts/tools/gen_cli_templates_4913.py templates/new src/cli_templates_4913.inc",
        )


if __name__ == "__main__":
    unittest.main()
