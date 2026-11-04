#!/usr/bin/env python3
"""Regression for #4916: an error inside a required module names its file.

The report used to carry no file name, the module's line:col, and a source
excerpt taken from the ENTRY file at the same line number. Every diagnostic
now reads "file:line:col" and the excerpt/caret come from the file the error
is in. A call that crosses files adds a short backtrace line
("at helper.aura:4:4 <- main.aura:4:2"). Covered on the direct, --load and
force-soa faces.
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

HELPER = ";; helper line 1\n;; helper line 2\n(define (bad x)\n  (memq x (list 1 2)))\n"
MAIN = '(require "./helper")\n\n(display "before") (newline)\n(bad 1)\n(display "after") (newline)\n'

FACES = (
    ("direct", [], {}),
    ("load", ["--load"], {}),
    ("force-soa", [], {"AURA_PIPELINE_STRICT": "force-soa"}),
)


def _run(argv_extra, extra_env, cwd, target):
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    env.update(extra_env)
    return subprocess.run(
        [str(AURA_BIN), *argv_extra, target],
        capture_output=True,
        text=True,
        env=env,
        cwd=cwd,
        timeout=60,
    )


class TestModuleDiagLocation4916(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name) / "loc"
        self.dir.mkdir()
        (self.dir / "helper.aura").write_text(HELPER)
        (self.dir / "main.aura").write_text(MAIN)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_module_error_names_module_file(self) -> None:
        for name, argv, env in FACES:
            with self.subTest(face=name):
                p = _run(argv, env, str(self.dir), "main.aura")
                self.assertNotEqual(p.returncode, 0, p.stderr)
                self.assertIn("helper.aura:4:4: unbound variable: memq", p.stderr)
                # Excerpt from helper.aura, not main.aura line 4 "(bad 1)".
                self.assertIn("(memq x (list 1 2)))", p.stderr)
                self.assertNotIn("| (bad 1)", p.stderr)
                self.assertIn("did you mean 'member'", p.stderr)
                self.assertIn("at helper.aura:4:4 \u2190 main.aura:4:2", p.stderr)
                self.assertEqual(p.stdout.count("before"), 1, p.stdout)

    def test_entry_error_names_entry_file(self) -> None:
        (self.dir / "e.aura").write_text('(display "x") (newline)\n(car 5)\n')
        for name, argv, env in FACES:
            with self.subTest(face=name):
                p = _run(argv, env, str(self.dir), "e.aura")
                self.assertNotEqual(p.returncode, 0, p.stderr)
                self.assertIn("e.aura:2:", p.stderr)
                self.assertEqual(p.stdout.count("x"), 1, p.stdout)

    def test_deny_after_std_require_is_in_entry_file(self) -> None:
        # The IR read node locations through the walker's last flat (the
        # loaded module), so a deny in the entry was shown at "io.aura:1:1"
        # and the program before it ran twice.
        src = '(require "std/io")\n(display "once") (newline)\n(file-copy "/etc/hostname" "/tmp/dx4916-copy.txt")\n'
        (self.dir / "d.aura").write_text(src)
        for name, argv, env in FACES:
            with self.subTest(face=name):
                env2 = dict(env)
                p = subprocess.run(
                    [str(AURA_BIN), *argv, "d.aura"],
                    capture_output=True,
                    text=True,
                    cwd=str(self.dir),
                    env={k: v for k, v in {**os.environ, **env2}.items() if k != "AURA_SANDBOX"},
                    timeout=60,
                )
                self.assertNotEqual(p.returncode, 0, p.stderr)
                self.assertIn("d.aura:3:2: effect-denied", p.stderr)
                self.assertNotIn("io.aura", p.stderr)
                self.assertEqual(p.stdout.count("once"), 1, p.stdout)


if __name__ == "__main__":
    unittest.main()
