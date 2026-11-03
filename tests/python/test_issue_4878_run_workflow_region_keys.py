#!/usr/bin/env python3
"""Regression for #4878: run-workflow multi-task stages under the production default.

The production default requires :region-keys for a 2+ task parallel-intend
(#3353). orch:run-workflow dropped the stage's region-keys and kept only the
bare status, so every multi-task stage was invalid with no reason and no way
to run. The stage hash's region-keys now reach parallel-intend, and the stage
result carries serialized-reason.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AURA_BIN = ROOT / "build" / "aura"

PROBE = r"""
(define (stage wf)
  (vector-ref (hash-ref wf "stages") 0))
(define (show tag wf)
  (let* ((s (stage wf)))
    (display (list tag (hash-ref wf "ok") (hash-ref wf "stages-ok")
                   (hash-ref s "status") (hash-ref s "serialized-reason")
                   (hash-ref s "region-keys-supplied")))
    (newline)))
(show 'keys (orch:run-workflow
             (vector (hash "tasks" (vector (lambda () 1) (lambda () 2))
                           "region-keys" (vector 1 2)))))
(show 'nokeys (orch:run-workflow
               (vector (hash "tasks" (vector (lambda () 1) (lambda () 2))))))
(show 'one (orch:run-workflow
            (vector (hash "tasks" (vector (lambda () 1))))))
"""


def _run(sandbox: str | None, timeout: float = 60.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(PROBE)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_SANDBOX", None)
    if sandbox is not None:
        env["AURA_SANDBOX"] = sandbox
    env["AURA_PIPELINE_STRICT"] = "0"
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
    finally:
        os.unlink(path)


class TestRunWorkflowRegionKeys4878(unittest.TestCase):
    def test_restricted_stage_with_keys_runs(self) -> None:
        p = _run(None)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(keys #t 1 ok  #t)", p.stdout)

    def test_restricted_stage_without_keys_reports_reason(self) -> None:
        p = _run(None)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(nokeys #f 0 invalid missing-or-overlap-keys #f)", p.stdout)

    def test_off_without_keys_runs(self) -> None:
        p = _run("off")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(nokeys #t 1 ok", p.stdout)


if __name__ == "__main__":
    unittest.main()
