#!/usr/bin/env python3
"""Issue #4862: --serve-async-bench must not hang on orch:agent-ask.

Root cause: the user-visible (fiber:yield) was REJECTED inside the soft
orch agent-body window, so a polling body hot-spun and pinned its worker
thread. The bench face auto-selects one worker (Soft, shared-Evaluator
safety), so the parked orch:agent-ask fiber could be resumed by neither
the reply push nor the :timeout-ms park deadline - the ask hung forever
(no reply AND no timeout).

The fix lets that one yield hand the turn back (the soft window holds no
workspace_mtx_ / rollback checkpoint and the yield re-queues the fiber as
Ready, it never parks), so the #4721 "no park inside the soft window"
contract is unchanged.
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

# The issue's own repro: an agent that polls its mailbox, asked from the
# main thread with a :timeout-ms.
BK1 = """(define (hg h k d) (if (hash-has-key? h k) (hash-ref h k) d))
(orch:spawn-agent "w" (lambda ()
  (let loop ((n 0))
    (let ((m (orch:agent-recv "w" :wait #f)))
      (if (eq? #t (hg m "ok" #f))
          (begin (orch:agent-reply (hg m "correlation-id" 0) "x") (loop (+ n 1)))
          (begin (fiber:yield) (loop n)))))) :attach-mailbox #t)
(display "spawned") (newline)
(define t0 (monotonic-ms))
(define r (orch:agent-ask "w" "q1" :timeout-ms 2000))
(display (list 'ask1 (- (monotonic-ms) t0) (hg r "ok" 'x) (hg r "status" 'x))) (newline)
(display "end") (newline)
"""


def _env() -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    return env


class TestIssue4862(unittest.TestCase):
    def test_bench_face_agent_ask_answers_and_does_not_hang(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bk1.aura"
            path.write_text(BK1, encoding="utf-8")

            # The bench face: this is where the hang lives.
            bench = subprocess.run(
                [str(AURA_BIN), "--serve-async-bench", str(path)],
                capture_output=True,
                text=True,
                env=_env(),
                timeout=60,
            )
            self.assertEqual(
                0,
                bench.returncode,
                f"bench face did not finish cleanly\n{bench.stdout[-500:]}\n{bench.stderr[-500:]}",
            )
            self.assertIn("end", bench.stdout, "bench face never reached the end")
            self.assertRegex(
                bench.stdout,
                r"\(ask1 \d+ #t ok\)",
                f"the answer did not arrive\n{bench.stdout[-500:]}",
            )

            # The script face must agree on the answer shape.
            loaded = subprocess.run(
                [str(AURA_BIN), "--load", str(path)],
                capture_output=True,
                text=True,
                env=_env(),
                timeout=60,
            )
            self.assertRegex(
                loaded.stdout,
                r"\(ask1 \d+ #t ok\)",
                f"the --load face did not answer\n{loaded.stdout[-500:]}",
            )


if __name__ == "__main__":
    unittest.main()
