#!/usr/bin/env python3
"""Regression for #4917: write-file failures are errors, success is 1.

Every failure path of write-file returned () with rc=0, so a write into a
missing directory looked like success. Failures now return an io error value
naming the path and the reason (uncaught in file mode: rc!=0), success
returns 1, and (make-directory path) creates the output directory without a
shell.
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


def _run(src: str, *, sandbox_off: bool = True, extra=()):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(src)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_PIPELINE_STRICT", None)
    if sandbox_off:
        env["AURA_SANDBOX"] = "off"
    else:
        env.pop("AURA_SANDBOX", None)
    try:
        return subprocess.run([str(AURA_BIN), *extra, path], capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


class TestWriteFileErrors4917(unittest.TestCase):
    def test_missing_directory_is_io_error(self) -> None:
        missing = tempfile.mkdtemp() + "/no-such-dir/x.txt"
        for extra in ((), ("--load",)):
            with self.subTest(extra=extra):
                p = _run(f'(define r (write-file "{missing}" "hi"))\n(display "next")\n', extra=extra)
                self.assertNotEqual(p.returncode, 0, p.stderr)
                self.assertIn("io error: write-file " + missing, p.stderr)
                self.assertIn("ENOENT", p.stderr)
                self.assertFalse(os.path.exists(missing))

    def test_caught_failure_is_error_value(self) -> None:
        missing = tempfile.mkdtemp() + "/nope/x.txt"
        p = _run(f'(display (try (write-file "{missing}" "hi") (catch (e) (list (quote caught) (format "~a" e)))))\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(caught write-file " + missing + ": ENOENT", p.stdout, p.stderr)

    def test_not_regular_and_protected(self) -> None:
        d = tempfile.mkdtemp()
        for target, needle in ((d, "write-file " + d), ("/proc/self/mem", "write-file /proc/self/mem")):
            with self.subTest(target=target):
                p = _run(f'(write-file "{target}" "x")\n')
                self.assertNotEqual(p.returncode, 0, p.stderr)
                self.assertIn(needle, p.stderr)

    def test_bad_arguments(self) -> None:
        p = _run('(write-file 42 "x")\n')
        self.assertNotEqual(p.returncode, 0, p.stderr)
        self.assertIn("expected (write-file path-string content)", p.stderr)

    def test_success_returns_one_and_make_directory(self) -> None:
        base = tempfile.mkdtemp()
        out = base + "/a/b/x.txt"
        p = _run(f'(display (make-directory "{base}/a/b")) (newline)\n(display (write-file "{out}" "hi")) (newline)\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.split(), ["1", "1"])
        self.assertEqual(Path(out).read_text(), "hi")

    def test_sandbox_deny_has_hint(self) -> None:
        p = _run('(write-file "/tmp/dx4917-deny.txt" "x")\n', sandbox_off=False)
        self.assertNotEqual(p.returncode, 0, p.stderr)
        self.assertIn("effect-denied", p.stderr)
        self.assertIn("hint:", p.stderr)


if __name__ == "__main__":
    unittest.main()
