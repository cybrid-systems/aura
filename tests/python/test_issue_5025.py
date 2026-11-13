#!/usr/bin/env python3
r"""Regression for #5025: the first screen is the terminal record.

Before: README, getting-started and install sent a new reader to
out/report.html, and \`aura new --help\` called evolve a "self-evolving program +
evo report". getting-started and install still name \`aura demo\` and the
KEEP / ROLLBACK record. The README first screen is the sum-evolve recording:
plain English, a wrong change undone, a same-answer change kept. The help
text and its snapshot move together.
"""

from __future__ import annotations

import os
import re
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
POINTERS = (
    ROOT / "docs" / "getting-started.md",
    ROOT / "docs" / "install.md",
)
SLOW = "(define (sum n) (slow-sum n))"
FAST = "(define (sum n) (quotient (* n (+ n 1)) 2))"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


class FirstScreen(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_docs_start_with_demo_and_the_record(self) -> None:
        for doc in POINTERS:
            text = doc.read_text()
            self.assertIn("aura demo", text, doc)
            self.assertIn(RECORD, text, doc)
            self.assertNotIn("out/report.html", text, doc)
            self.assertNotIn("aura new evolve demo\ncd demo\naura run", text, doc)

    def test_readme_is_the_recording(self) -> None:
        text = (ROOT / "README.md").read_text()
        self.assertIn("aura --preset dev --quiet examples/sum-evolve.aura", text)
        self.assertIn("docs/assets/sum-evolve.gif", text)
        self.assertIn("docs/assets/sum-evolve.cast", text)
        self.assertIn(
            "Let AI rewrite your running program — safely. "
            "Bad changes are undone instantly; good ones go live without a restart.",
            text,
        )
        self.assertIn(
            "curl -fsSL https://raw.githubusercontent.com/cybrid-systems/aura/main/install.sh | sh",
            text,
        )
        self.assertNotIn("out/report.html", text)
        self.assertNotIn(RECORD, text)
        self.assertNotIn("aura new evolve demo\ncd demo\naura run", text)

    def test_sum_evolve_keeps_only_a_matching_result(self) -> None:
        os.environ["AURA_SUM_EVOLVE_PACE"] = "0"
        try:
            r = run(
                ["--preset", "dev", "--quiet", str(ROOT / "examples" / "sum-evolve.aura")],
                ROOT,
            )
        finally:
            os.environ.pop("AURA_SUM_EVOLVE_PACE", None)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        text = ANSI.sub("", r.stdout)
        self.assertIn("A program is running. It has a slow function.", text)
        self.assertLess(text.index("Wrong answer"), text.index("Same answer"))
        self.assertIn("AI suggests a change…", text)
        self.assertIn("AI suggests another change…", text)
        self.assertIn("Testing in a sandbox…", text)
        self.assertIn("(define (sum n) (* n n))", text)
        self.assertIn(FAST, text)
        after_wrong = text.split("Wrong answer", 1)[1].split("AI suggests another", 1)[0]
        self.assertIn(f"before  {SLOW}", after_wrong)
        self.assertNotIn(FAST, after_wrong)
        self.assertIn(f"after   {FAST}", text)
        self.assertIn("The program never stopped.", text)
        self.assertNotIn("letrec", text)
        timed = re.search(r"Same answer, (\d+) ms → (\d+) ms → kept, swapped in live", text)
        self.assertIsNotNone(timed, text)
        self.assertGreaterEqual(int(timed.group(1)), int(timed.group(2)))

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
