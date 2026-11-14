#!/usr/bin/env python3
"""Regression for #4904: orch:run-workflow / parallel-intend under force-soa.

Under AURA_PIPELINE_STRICT=force-soa top-level defines run on the IR.

* A user function that calls orch:run-workflow (or parallel-intend) hung
  forever: the CLI main thread ran the function body holding that define's
  interpreter mutex and blocked in the batch join, while each task thunk
  (minted by that same body) dispatched back into the interpreter on a
  worker and waited for the mutex.
* (define r (orch:run-workflow ...)) returned stage status partial / ok #f:
  the value define's interpreter was registered as the owner of the
  closures it minted only after the value was computed, so the thunks
  could not be applied while the batch ran ("apply-failed").
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

FACES = [(s, d) for s in ("force-soa", "0", "") for d in (True, False)]

WF = '(vector (hash "tasks" (vector (lambda () 1) (lambda () (+ 1 1)))))'


def _run(source: str, direct: bool, strict: str, sandbox: str | None = "off"):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    if sandbox is None:
        env.pop("AURA_SANDBOX", None)
    else:
        env["AURA_SANDBOX"] = sandbox
    if strict:
        env["AURA_PIPELINE_STRICT"] = strict
    else:
        env.pop("AURA_PIPELINE_STRICT", None)
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=30)
    except subprocess.TimeoutExpired:
        return None
    finally:
        os.unlink(path)


CASES = {
    "workflow_in_user_fn_statement": (
        f'(define (go) (orch:run-workflow {WF}))\n(display "pre") (newline)\n(go)\n(display "post") (newline)\n',
        "pre\npost",
    ),
    "workflow_in_user_fn": (
        f"(define (go) (orch:run-workflow {WF}))\n"
        '(display "pre") (newline)\n'
        "(define r (go))\n"
        '(display (list (hash-ref r "stages-ok") (hash-ref r "stages-failed")))\n'
        '(newline) (display "post") (newline)\n',
        "pre\n(1 0)\npost",
    ),
    "workflow_value_define": (
        f"(define r (orch:run-workflow {WF}))\n"
        '(define s (vector-ref (hash-ref r "stages") 0))\n'
        '(display (list (hash-ref s "status") (hash-ref s "ok")\n'
        '               (hash-ref r "stages-ok") (hash-ref r "stages-failed")))\n'
        "(newline)\n",
        "(ok #t 1 0)",
    ),
    "parallel_intend_in_user_fn": (
        "(define (f) (parallel-intend (vector (lambda () 1) (lambda () 2))))\n"
        '(display (hash-ref (f) "status")) (newline)\n'
        "(define v (f))\n"
        '(display (hash-ref v "status")) (newline)\n',
        "ok\nok",
    ),
    "parallel_intend_value_define": (
        "(define r (parallel-intend (vector (lambda () 1) (lambda () 2))))\n"
        '(display (list (hash-ref r "status") (hash-ref r "ok-count")))\n'
        "(newline)\n",
        "(ok 2)",
    ),
}


class TestForceSoaWorkflow4904(unittest.TestCase):
    def test_all_faces(self) -> None:
        # Sandbox off: every face must produce the expected result. Sandbox
        # unset (production default): the orchestration face may refuse the
        # batch by policy, but every face must agree with the default whole-
        # file face and none may hang.
        failures = []
        for name, (src, want) in CASES.items():
            for sandbox in ("off", None):
                base = None
                for strict, direct in [("", True)] + [f for f in FACES if f != ("", True)]:
                    p = _run(src, direct, strict, sandbox)
                    tag = f"{name} strict={strict or 'default'} direct={direct} sb={sandbox}"
                    if p is None:
                        failures.append(f"{tag}: timed out (hang)")
                        continue
                    out = p.stdout.strip()
                    if base is None:
                        base = out
                    expect = want if sandbox == "off" else base
                    if p.returncode != 0 or out != expect:
                        failures.append(f"{tag}: rc={p.returncode} out={out!r} want={expect!r} err={p.stderr[-300:]!r}")
        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main()
