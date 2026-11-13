#!/usr/bin/env python3
r"""Regression for #5025: the first screen is the terminal record.

Before: README, getting-started and install sent a new reader to
out/report.html, and \`aura new --help\` called evolve a "self-evolving program +
evo report". getting-started and install still name \`aura demo\` and the
KEEP / ROLLBACK record. The README shows that record as a cast of
examples/sum-to.aura (one ROLLBACK, one KEEP, same process). The help text
and its snapshot move together.
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


def run(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        env.pop(k, None)
    return subprocess.run(
        [str(AURA_BIN), *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
        stdin=subprocess.DEVNULL,
    )


RECORD = "1 ROLLBACK  (define (guess) 10)   live (define (guess) 0)"
DOCS = (
    ROOT / "docs" / "getting-started.md",
    ROOT / "docs" / "install.md",
)


class FirstScreen(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_docs_start_with_demo_and_the_record(self) -> None:
        for doc in DOCS:
            text = doc.read_text()
            self.assertIn("aura demo", text, doc)
            self.assertIn(RECORD, text, doc)
            self.assertNotIn("out/report.html", text, doc)
            self.assertNotIn("aura new evolve demo\ncd demo\naura run", text, doc)
        readme = (ROOT / "README.md").read_text()
        self.assertIn("docs/assets/sum-to.gif", readme)
        self.assertIn("docs/assets/sum-to.cast", readme)
        self.assertIn("aura --preset dev examples/sum-to.aura", readme)
        self.assertIn(
            "curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh",
            readme,
        )
        self.assertNotIn("out/report.html", readme)
        self.assertNotIn("aura new evolve demo\ncd demo\naura run", readme)

    def test_new_help_describes_keep_rollback(self) -> None:
        r = run(["new", "--help"], self.d)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("KEEP / ROLLBACK", r.stdout)
        self.assertNotIn("evo report", r.stdout)

    def test_top_help_first_example_is_demo(self) -> None:
        r = run(["--help"], self.d)
        lines = r.stdout.splitlines()
        demo_at = next(i for i, ln in enumerate(lines) if "aura demo" in ln)
        file_at = next(i for i, ln in enumerate(lines) if "file.aura" in ln)
        self.assertLess(demo_at, file_at, r.stdout)

    def test_demo_help(self) -> None:
        for args in (["demo", "--help"], ["help", "demo"]):
            r = run(args, self.d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("usage: aura demo", r.stdout)
            self.assertIn("agent", r.stdout)


if __name__ == "__main__":
    unittest.main()
