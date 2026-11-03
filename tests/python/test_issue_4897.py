#!/usr/bin/env python3
"""Issue #4897: orch:run-workflow inside an agent body hung the bench.

Under AURA_SANDBOX=off + --serve-async-bench (Soft auto: one worker since
#4877) an agent body that called orch:run-workflow never returned and the
whole process hung without output. parallel-intend joins its stage task
with an untimed Fiber::join; inside the soft orch agent-body window the
BlockingIO park is rejected (#4721), so the join returned from every park
at once and busy-spun the only worker. The stage task sat on that same
worker and never ran.
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

# A multi-task stage needs :region-keys under the production default
# (#4878), so the production run uses single-task stages.
AGENT_WF = """(orch:spawn-agent "wfa" (lambda ()
  (display (list 'in-agent (hash-ref (orch:run-workflow
     (vector (hash "tasks" (vector (lambda () 1)))
             (hash "tasks" (vector (lambda () 3))))) "ok")))
  (newline) 'ok))
(display (list 'join (hash-ref (orch:agent-join "wfa" :timeout-ms 3000) "ok"))) (newline)
(display "end") (newline)
"""

FIBER_WF = """(define f (fiber:spawn (lambda ()
  (hash-ref (orch:run-workflow (vector (hash "tasks" (vector (lambda () 1))))) "ok"))))
(display (list 'fiber (fiber:join f))) (newline)
(display "end") (newline)
"""


def _env(soft: bool) -> dict[str, str]:
    env = dict(os.environ)
    env.pop("AURA_PIPELINE_STRICT", None)
    if soft:
        env["AURA_SANDBOX"] = "off"
    else:
        env.pop("AURA_SANDBOX", None)
    return env


class TestIssue4897(unittest.TestCase):
    def _run(self, src: str, soft: bool, extra: list[str]) -> subprocess.CompletedProcess:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "wf.aura"
            path.write_text(src, encoding="utf-8")
            try:
                return subprocess.run(
                    [str(AURA_BIN), "--serve-async-bench", str(path), *extra],
                    capture_output=True,
                    text=True,
                    env=_env(soft),
                    timeout=20,
                )
            except subprocess.TimeoutExpired:
                self.fail(f"soft={soft} {extra}: bench hung (orch:run-workflow in agent)")

    def _check_agent(self, soft: bool, extra: list[str]) -> None:
        for _ in range(3):
            p = self._run(AGENT_WF, soft, extra)
            self.assertEqual(0, p.returncode, p.stderr[-800:])
            self.assertIn("(in-agent #t)", p.stdout)
            self.assertIn("(join #t)", p.stdout)
            self.assertIn("end", p.stdout)

    def test_agent_run_workflow_single_worker_soft(self) -> None:
        self._check_agent(True, [])

    def test_agent_run_workflow_production_default(self) -> None:
        self._check_agent(False, [])

    def test_agent_run_workflow_multi_worker(self) -> None:
        self._check_agent(True, ["--worker-threads=2"])

    def test_fiber_run_workflow_single_worker_soft(self) -> None:
        p = self._run(FIBER_WF, True, [])
        self.assertEqual(0, p.returncode, p.stderr[-800:])
        self.assertIn("(fiber #t)", p.stdout)


if __name__ == "__main__":
    unittest.main()
