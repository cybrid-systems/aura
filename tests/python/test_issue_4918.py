#!/usr/bin/env python3
"""Regression for #4918: "./x" requires resolve against the requiring file.

A relative require resolved against the process cwd, so `aura loc/main.aura`
failed from the parent directory, the failure was labelled internal error,
named no searched paths, and on the IR faces a miss was silent (rc=0). Now
"./" and "../" resolve against the requiring file's directory (the entry
file for the top level, nested modules against their own directory), and a
miss is a located "module error: require: cannot resolve ... (searched: ...)".
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

FACES = (
    ("direct", [], {}),
    ("load", ["--load"], {}),
    ("force-soa", [], {"AURA_PIPELINE_STRICT": "force-soa"}),
)


def _run(argv, env_extra, cwd):
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    env.update(env_extra)
    return subprocess.run([str(AURA_BIN), *argv], capture_output=True, text=True, env=env, cwd=cwd, timeout=60)


class TestRelativeRequire4918(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.root = root
        (root / "kit" / "lib").mkdir(parents=True)
        (root / "kit" / "lib" / "util.aura").write_text("(define (twice x) (* 2 x))\n")
        # nested: lib/mid.aura requires its sibling with "./util"
        (root / "kit" / "lib" / "mid.aura").write_text('(require "./util")\n(define (quad x) (twice (twice x)))\n')
        (root / "kit" / "main.aura").write_text('(require "./lib/mid")\n(display (quad 3)) (newline)\n')
        (root / "kit" / "miss.aura").write_text('(require "./nothere")\n(display "after") (newline)\n')

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_any_cwd(self) -> None:
        for cwd, target in (
            (self.root / "kit", "main.aura"),
            (self.root, "kit/main.aura"),
            (Path("/"), str(self.root / "kit" / "main.aura")),
        ):
            for name, argv, env in FACES:
                with self.subTest(cwd=str(cwd), face=name):
                    p = _run([*argv, target], env, str(cwd))
                    self.assertEqual(p.returncode, 0, p.stderr)
                    self.assertEqual(p.stdout.strip(), "12")

    def test_miss_is_module_error_with_search_list(self) -> None:
        want = str((self.root / "kit" / "nothere.aura").resolve())
        for name, argv, env in FACES:
            with self.subTest(face=name):
                p = _run([*argv, "kit/miss.aura"], env, str(self.root))
                self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertIn("module error: require: cannot resolve './nothere'", p.stderr)
                self.assertIn("searched: " + want, p.stderr)
                self.assertNotIn("internal error", p.stderr)
                self.assertIn("miss.aura:1:2", p.stderr)


if __name__ == "__main__":
    unittest.main()
