#!/usr/bin/env python3
"""Issue #4856: --serve-async-bench and --load must agree on the SAME script.

The bench face (--serve-async-bench) and the script face (--load) run the
same .aura file. Before #4856 they disagreed:

  * a failing agent body was reported as ok / reclaimed / invalid under the
    bench: the bench fiber carried no owner scheduler, so the orch spawn
    path (#4843) put the agent body on a SECOND scheduler and the body
    never ran;
  * scope-join-all reported "invalid" (no body ran) instead of "partial";
  * an uncaught error lost its source location (row:col) and the source
    snippet: the bench split the file into one program per form, so the
    diagnostic was anchored to the split expression, not to the file.

Every case below runs the same script under both faces and compares the
semantic result (stdout, exit status, and the located diagnostic).
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def _env() -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    # The script face needs the permissive walker face (#2213 gate); the
    # bench face takes it on its own (#4844).
    env["AURA_PIPELINE_STRICT"] = "0"
    return env


def _run(source: str, mode: str, timeout: float = 60.0) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    try:
        return subprocess.run(
            [str(AURA_BIN), mode, path],
            capture_output=True,
            text=True,
            env=_env(),
            timeout=timeout,
        )
    finally:
        os.unlink(path)


def _located_diagnostic(stderr: str) -> str:
    """The located diagnostic of a failed run: "row:col: ..." + snippet.

    The bench prefixes the diagnostic with "eval error: " and --load with
    "error: "; everything from the "row:col:" marker on is the diagnostic
    itself and must be identical in both faces.
    """
    lines = stderr.splitlines()
    for index, line in enumerate(lines):
        marker = re.search(r"\d+:\d+: ", line)
        if marker:
            return "\n".join([line[marker.start() :]] + lines[index + 1 :])
    return ""


class TestIssue4856DualModeConsistency(unittest.TestCase):
    """The same script must behave the same under --load and --serve-async-bench."""

    def _assert_same(self, source: str, expect_stdout: list[str] | None = None) -> None:
        load = _run(source, "--load")
        bench = _run(source, "--serve-async-bench")
        self.assertEqual(load.returncode, bench.returncode, f"exit status differs: {load.stderr} / {bench.stderr}")
        self.assertEqual(load.stdout, bench.stdout, f"stdout differs: {load.stdout!r} / {bench.stdout!r}")
        for expected in expect_stdout or []:
            self.assertIn(expected, load.stdout)
            self.assertIn(expected, bench.stdout)

    def test_4856_agent_body_failure_is_not_ok(self) -> None:
        # The minimal repro of the issue: a raising body and an erroring body
        # must both join as an error, never as ok / reclaimed / invalid.
        self._assert_same(
            """
(orch:spawn-agent "a" (lambda () (raise 'my-symbol)))
(orch:spawn-agent "e" (lambda () (error "plain message")))
(define (show n) (let ((j (orch:agent-join n :timeout-ms 3000)))
  (display (list n (hash-ref j "ok") (hash-ref j "status") (hash-ref j "error"))) (newline)))
(show "a")
(show "e")
""",
            expect_stdout=["(a #f error my-symbol)", "(e #f error plain message)"],
        )

    def test_4856_scope_join_all_status_matches(self) -> None:
        # A failing scope body: the join-all aggregate is "partial" with one
        # error in both faces (the bench used to report "invalid" with 0
        # errors because no body ever ran).
        self._assert_same(
            """
(orch:scope-spawn "failing" (lambda () (error "scope boom")))
(define r (orch:scope-join-all :timeout-ms 3000))
(display (list (hash-ref r "ok") (hash-ref r "status") (hash-ref r "err-count")
               (hash-ref r "body-ok-count"))) (newline)
""",
            expect_stdout=["(#f partial 1 0)"],
        )

    def test_4856_nested_scope_in_agent_body_matches(self) -> None:
        # A scope spawned from inside an agent body (#4842): both faces agree.
        self._assert_same(
            """
(orch:spawn-agent "outer" (lambda () (orch:scope-spawn "inner" (lambda () (error "inner boom")))))
(define j (orch:agent-join "outer" :timeout-ms 3000))
;; Issue #4855: hash-ref without a default RAISES on a missing key, so the
;; optional "error" field needs an explicit default here.
(display (list (hash-ref j "ok") (hash-ref j "status") (hash-ref j "error" '-'))) (newline)
(define r (orch:scope-join-all :timeout-ms 3000))
(display (list (hash-ref r "ok") (hash-ref r "status") (hash-ref r "err-count"))) (newline)
""",
            expect_stdout=["(#t ok -)", "(#f partial 1)"],
        )

    def test_4856_successful_agent_matches(self) -> None:
        # The happy path must stay identical too.
        self._assert_same(
            """
(orch:spawn-agent "m" (lambda () 1))
(define j (orch:agent-join "m" :timeout-ms 3000))
(display (list (hash-ref j "ok") (hash-ref j "status"))) (newline)
""",
            expect_stdout=["(#t ok)"],
        )

    def test_4856_uncaught_error_keeps_location_and_snippet(self) -> None:
        # An uncaught type error keeps its source location (row:col) and the
        # source snippet in BOTH faces.
        source = """
(define (add-cost total item)
  (+ total item))

(add-cost 1 "two")
"""
        load = _run(source, "--load")
        bench = _run(source, "--serve-async-bench")
        self.assertEqual(load.returncode, 1, load.stderr)
        self.assertEqual(bench.returncode, 1, bench.stderr)
        load_diag = _located_diagnostic(load.stderr)
        bench_diag = _located_diagnostic(bench.stderr)
        self.assertTrue(load_diag, f"no located diagnostic in --load: {load.stderr!r}")
        self.assertEqual(load_diag, bench_diag, f"diagnostic differs: {load_diag!r} / {bench_diag!r}")
        # Issue #4855: the diagnostic points at the INNER failing form
        # (the "+" application), not at the outer call site.
        inner_line = next(i for i, line in enumerate(source.splitlines(), 1) if "+ total item" in line)
        inner_col = source.splitlines()[inner_line - 1].index("+") + 1
        self.assertIn(f"{inner_line}:{inner_col}: type error:", load_diag)
        # The source snippet and the caret line travel with the diagnostic.
        self.assertIn("(+ total item)", load_diag)
        self.assertIn("^", load_diag)
        # The failing operand is named (#4855) in both faces.
        self.assertIn("in +, argument 2", load_diag)

    def test_4856_bench_failure_keeps_the_eval_error_marker(self) -> None:
        # The bench keeps its "eval error" marker (#4844: an eval failure must
        # not exit 0) and now also carries the located diagnostic.
        bench = _run("(car 5)\n", "--serve-async-bench")
        self.assertNotEqual(bench.returncode, 0, "eval failure must not exit 0")
        self.assertIn("eval error", bench.stderr)


if __name__ == "__main__":
    unittest.main()
