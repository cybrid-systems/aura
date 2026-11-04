#!/usr/bin/env python3
"""Regression for #4919: requiring a std module needs no effect.

Under the default (Restricted) sandbox `(require "std/fs")` failed at load
because the module also has write functions; std/process needed exec. The
effect check now happens when a write / exec function is called, and its
refusal is an effect-denied with a hint. file-exists? has one definition
returning Bool in the primitive, the type checker and std/fs.
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


def _run(src: str, argv=(), env_extra=None):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(src)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_SANDBOX", None)  # default sandbox
    env.pop("AURA_PIPELINE_STRICT", None)
    env.update(env_extra or {})
    try:
        return subprocess.run([str(AURA_BIN), *argv, path], capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


class TestStdRequireNoEffect4919(unittest.TestCase):
    def test_require_fs_and_read_only_calls(self) -> None:
        src = (
            '(require "std/fs")\n(require "std/process")\n'
            '(display (file-exists? "/etc/hostname")) (newline)\n'
            '(display (file-exists? "/nonexistent-dx4919")) (newline)\n'
            '(display (boolean? (file-exists? "/etc/hostname"))) (newline)\n'
        )
        for name, argv, env in FACES:
            with self.subTest(face=name):
                p = _run(src, argv, env)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(p.stdout.split(), ["#t", "#f", "#t"])
                self.assertNotIn("effect-denied", p.stderr)

    def test_write_call_denied_with_hint(self) -> None:
        src = '(require "std/fs")\n(file-write "/tmp/dx4919.txt" "x")\n'
        for name, argv, env in FACES:
            with self.subTest(face=name):
                p = _run(src, argv, env)
                self.assertNotEqual(p.returncode, 0, p.stderr)
                self.assertIn("effect-denied: write not granted", p.stderr)
                self.assertIn("hint:", p.stderr)
                self.assertNotIn("module-load-failed", p.stderr)

    def test_file_copy_needs_write_not_exec(self) -> None:
        p = _run('(require "std/io")\n(file-copy "/etc/hostname" "/tmp/dx4919-c.txt")\n')
        self.assertIn("effect-denied: write not granted", p.stderr)
        self.assertNotIn("exec not granted", p.stderr)

    def test_shell_denied_at_call(self) -> None:
        p = _run('(require "std/process")\n(display "loaded") (newline)\n(shell "true")\n')
        self.assertIn("loaded", p.stdout)
        self.assertIn("effect-denied: exec not granted", p.stderr)
        self.assertIn("hint:", p.stderr)

    def test_bool_type_with_eq(self) -> None:
        p = _run(
            '(require "std/fs")\n(display (eq? (file-exists? "/etc/hostname") #t))\n', env_extra={"AURA_SANDBOX": "off"}
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), "#t")


if __name__ == "__main__":
    unittest.main()
