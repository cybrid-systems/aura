#!/usr/bin/env python3
"""Regression for #5049: a bare REPL printed <error> and lost its session.

Two faults: the REPL printer replaced every error VALUE with a bare
<error>, hiding the cause the file faces already read through
soft_error_message; and a bare `aura` on a TTY did not share the
`aura repl` soft session / std prelude, so `set-code` was denied.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

import pexpect

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN, repl_value  # noqa: E402

GRANT = "aura: repl preset=dev soft auto-grant: mutate,read"
SET_CODE = '(set-code "(define params (quote (0 0)))")'


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    return e


def run_repl(extra: list[str], stdin: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(AURA_BIN), "repl", *extra],
        input=stdin,
        capture_output=True,
        text=True,
        env=env(),
        timeout=120,
    )


class ErrorValueCause(unittest.TestCase):
    # Before: stdout was a single "<error>" line; the cause was dropped.
    def test_primitive_error_value_prints_its_cause(self) -> None:
        r = run_repl([], "(car 5)\n(quit)\n")
        self.assertEqual(repl_value(r.stdout.strip()), "car: not a pair")
        self.assertNotIn("<error>", r.stdout)

    def test_denied_set_code_prints_the_deny_reason(self) -> None:
        r = run_repl(["--preset", "prod"], SET_CODE + "\n(quit)\n")
        self.assertEqual(repl_value(r.stdout.strip()), "effect-denied: mutate not granted tenant=0 op=set-code")


class BareTtySession(unittest.TestCase):
    # Before: bare `aura` stayed on the Restricted default with no grant,
    # so `set-code` was refused and only std/list-less bindings resolved.
    def test_bare_tty_shares_the_repl_soft_session(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), [], timeout=60, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(GRANT)
            child.sendline(SET_CODE)
            child.expect("#t")
            # std/list-only name: resolves only with the `aura repl` prelude.
            child.sendline('(string-join (list "a" "b") "-")')
            child.expect("a-b")
        finally:
            child.sendline("(quit)")
            child.close(force=True)


class BarePipeOneshot(unittest.TestCase):
    # #2322: a piped bare `aura` stays the whole-program oneshot.
    def test_bare_pipe_stays_oneshot(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN)],
            input=SET_CODE + "\n",
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        combined = r.stdout + r.stderr
        self.assertNotIn("#t", r.stdout)
        self.assertNotIn(GRANT, combined)
        self.assertIn("effect-denied: mutate not granted", combined)
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
