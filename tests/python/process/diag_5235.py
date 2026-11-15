#!/usr/bin/env python3
"""Issue #5235: arity/location and CLI-face diagnostics."""

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


def run(args: list[str], cwd: str | None = None, sandbox: str = "off") -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    if sandbox:
        env["AURA_SANDBOX"] = sandbox
    env.pop("AURA_IR_PIPELINE_TRACE", None)
    return subprocess.run([str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120)


class Diag5235(unittest.TestCase):
    def test_if_requires_branches(self) -> None:
        missing = run(["-e", "(if)"])
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("(if cond then [else])", missing.stderr)
        self.assertIn("1:4", missing.stderr)
        one_arm = run(["-e", "(if 1)"])
        self.assertNotEqual(one_arm.returncode, 0)
        self.assertIn("(if cond then [else])", one_arm.stderr)
        self.assertIn("1:6", one_arm.stderr)

    def test_zero_arg_check_is_a_call(self) -> None:
        # #5301: `(check)` is a call. A null condition used to make #5235
        # reject `(if (check) …)` inside a fiber lambda and at top level.
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "c.aura"
            p.write_text(
                "(define (check) #t)\n"
                "(display (if (check) 1 0))\n"
                "(display ((lambda () (if (check) 1 0))))\n"
                "(display (fiber:join (fiber:spawn (lambda () (if (check) 1 0)))))\n"
            )
            r = run(["--quiet", str(p)], cwd=d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout, "111")

        bare = run(["-e", "(check)"])
        self.assertNotEqual(bare.returncode, 0)
        self.assertIn("check failed: expected a truthy value, got no argument", bare.stderr)
        self.assertNotIn("requires a condition", bare.stderr)

    def test_define_missing_name_has_location(self) -> None:
        r = run(["-e", "(define)"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("requires a name", r.stderr)
        self.assertIn("1:8", r.stderr)

    def test_jit_reads_file_argument(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "p.aura"
            p.write_text("(display (car 5))\n")
            r = run(["--jit", str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("car: not a pair", r.stderr)
            self.assertNotIn("unbound variable", r.stderr)

    def test_short_name_gets_no_noise_suggestion(self) -> None:
        r = run(["-e", "(baz)"])
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn("did you mean", r.stderr)

    def test_memq_keeps_its_useful_hint(self) -> None:
        r = run(["-e", "(memq 1 (list 1))"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("member", r.stderr)

    def test_macro_deny_has_location(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.aura"
            p.write_text("(define-hygienic-macro (twice e) (+ e e))\n(display (twice 3))\n")
            r = run([str(p)], cwd=d, sandbox="restricted")
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("macro-self-evo not granted", r.stderr)
            self.assertIn("m.aura:1:2", r.stderr)
            self.assertIn("^", r.stderr)

    def test_check_failure_names_actual_value(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "c.aura"
            p.write_text("(check (= 1 2))\n")
            r = run([str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("check failed: expected a truthy value, got #f", r.stderr)
            self.assertIn("c.aura:1:2", r.stderr)
            self.assertNotIn("type error: <error>", r.stderr)

    def test_check_equal_failure_names_both_values(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "c.aura"
            p.write_text("(check= 1 2)\n")
            r = run([str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("check= failed: expected 2, got 1", r.stderr)

    def test_string_literal_column_unit_is_bytes(self) -> None:
        # #5235: the lexer advanced by the DECODED string length, so each
        # preceding string literal lost its two quotes (plus one per escape).
        cases = {
            '"ab"': 26,
            '"λ"': 26,  # 2-byte lambda: byte column
            '"漢漢"': 30,  # two 3-byte chars
            '"a\\"b"': 28,
        }
        with tempfile.TemporaryDirectory() as d:
            for lit, col in cases.items():
                p = Path(d) / "s.aura"
                p.write_text(f"(define s {lit}) (display zz)\n")
                r = run([str(p)], cwd=d)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn(f"s.aura:1:{col}: unbound variable: zz", r.stderr, lit)

    def test_bool_literal_column_unit_is_raw_span(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "b.aura"
            p.write_text("(define b #t) (display zz)\n")
            r = run([str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("b.aura:1:24: unbound variable: zz", r.stderr)

    def test_set_code_type_warning_names_workspace_source(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "probe.aura"
            p.write_text('(define src "(: x Int \\"s\\")")\n(set-code src)\n(display (typecheck-current))\n')
            r = run([str(p)], cwd=d, sandbox="off")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("workspace.aura:1:10", r.stdout)
            self.assertIn("expected Int, got String", r.stdout)
            self.assertNotIn("probe.aura:1:10", r.stdout)

    def test_typecheck_memq_hint_matches_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "q.aura"
            p.write_text("(display (memq 1 (list 1)))\n")
            runtime = run([str(p)], cwd=d)
            checked = run(["--typecheck", str(p)], cwd=d)
            self.assertNotEqual(runtime.returncode, 0)
            self.assertNotEqual(checked.returncode, 0)
            hint = "did you mean 'member'? (Aura's member compares with equal?; memq/memv are not provided)"
            self.assertIn(hint, runtime.stderr)
            # --typecheck renders the diagnostic (and its suggestion) on stdout.
            self.assertIn(hint, checked.stdout)
            self.assertNotIn("mean'", checked.stdout)

    def test_ir_error_exits_nonzero_without_pipeline_trace(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "p.aura"
            p.write_text("(display (car 5))\n")
            r = run(["--ir", str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("car: not a pair", r.stderr)
            self.assertNotIn("PM: running", r.stderr)


if __name__ == "__main__":
    unittest.main()
