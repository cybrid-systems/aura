#!/usr/bin/env python3
"""Regression for #5172: a serve error value is status=error.

The sync s-expression line already reported `(car (list))` as an error.
JSON `exec` and `query` printed `{"status":"ok","value":"<error>"}` and
dropped the cause. They now use the same sync error line. A program that
returns the string "<error>" stays status=ok. AURA_SANDBOX stays unset.
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

CAR = "(car (list))\n"
EXEC = '{"cmd":"exec","code":"(car (list))"}\n'
QUERY = '{"cmd":"query","code":"(car (list))"}\n'
OK = '{"cmd":"exec","code":"(+ 1 2)"}\n'
STRING = '"<error>"\n'
STRING_JSON = '{"cmd":"exec","code":"\\"<error>\\""}\n'
CAUSE = "car: not a pair"


def run_aura(args: list[str], text: str, timeout: float = 20) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    with tempfile.TemporaryDirectory() as tmp:
        proc = subprocess.Popen(
            [str(AURA_BIN), *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=tmp,
            env=env,
            text=True,
        )
        try:
            out, err = proc.communicate(text, timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
        return subprocess.CompletedProcess(proc.args, proc.returncode, out, err)


class TestIssue5172ServeErrorValue(unittest.TestCase):
    def test_car_is_an_error_on_every_sync_path(self) -> None:
        sexp = run_aura(["--serve"], CAR)
        exe = run_aura(["--serve"], EXEC)
        query = run_aura(["--serve"], QUERY)
        for proc in (sexp, exe, query):
            blob = proc.stdout + proc.stderr
            self.assertEqual(proc.returncode, 0, blob)
            self.assertIn('"status":"error"', proc.stdout)
            self.assertIn(CAUSE, proc.stdout)
            self.assertNotIn('"value":"<error>"', proc.stdout)
        self.assertEqual(sexp.stdout, exe.stdout)
        self.assertEqual(sexp.stdout, query.stdout)

    def test_serve_async_reports_the_same_failure(self) -> None:
        # --serve-async prints the line and then may keep the scheduler up
        # after stdin EOF. The protocol line is the contract; kill a process
        # that has already answered.
        proc = run_aura(["--serve-async"], EXEC, timeout=8)
        blob = proc.stdout + proc.stderr
        self.assertIn('"status":"error"', proc.stdout, blob)
        self.assertIn(CAUSE, proc.stdout, blob)
        self.assertIn('"session":"default"', proc.stdout, blob)
        self.assertNotIn('"value":"<error>"', proc.stdout)

    def test_string_error_text_stays_ok(self) -> None:
        sexp = run_aura(["--serve"], STRING)
        exe = run_aura(["--serve"], STRING_JSON)
        for proc in (sexp, exe):
            blob = proc.stdout + proc.stderr
            self.assertEqual(proc.returncode, 0, blob)
            self.assertIn('"status":"ok"', proc.stdout)
            self.assertIn("<error>", proc.stdout)
            self.assertNotIn('"status":"error"', proc.stdout)

    def test_successful_exec_stays_ok(self) -> None:
        proc = run_aura(["--serve"], OK)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn('{"status":"ok","value":"3"}', proc.stdout)
        self.assertNotIn('"status":"error"', proc.stdout)


if __name__ == "__main__":
    unittest.main()
