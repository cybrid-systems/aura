#!/usr/bin/env python3
"""Issue #4887/#4888: offload:* primitives and eval:async on the offload pool.

The snapshot is the live workspace source; submit runs typecheck + test in a
thread-private CompilerService and await returns the result hash. Non-fiber
contexts (plain aura reading stdin) must degrade to mode: sync.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

PROGRAM = r"""
(begin
  (set-code "(require \"std/test\" all:)(test-suite \"basic\" (check= 3 (+ 1 2)))")
  (eval-current)
  (let ((r (offload:await
             (offload:submit (offload:snapshot) '(typecheck test) :class 'interactive))))
    (let ((tests (hash-ref r "tests")))
      (list (hash-ref r "status") (hash-ref r "mode")
            (hash-ref tests "passed") (hash-ref tests "failed")))))
"""


class TestOffloadPrimitives(unittest.TestCase):
    def test_offload_sync_in_plain_aura(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        proc = subprocess.run(
            [str(AURA_BIN)],
            input=PROGRAM,
            text=True,
            capture_output=True,
            timeout=60,
            env=env,
        )
        out = proc.stdout + proc.stderr
        self.assertIn(":done", out, out[-1000:])
        self.assertIn(":sync", out, out[-1000:])

    def test_offload_async_in_serve_async(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        # The session's own (set-code ...) tree-walks; production Forbidden
        # rejects that before the offload call (pre-existing #2213 gate).
        # force-soa still exercises the private offload context.
        env["AURA_PIPELINE_STRICT"] = "force-soa"
        payload = json.dumps({"session": "default", "cmd": "exec", "code": PROGRAM}) + "\n"
        proc = subprocess.run(
            [str(AURA_BIN), "--serve-async"],
            input=payload,
            text=True,
            capture_output=True,
            timeout=60,
            env=env,
        )
        rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(len(rows), 1, f"stdout={proc.stdout[-800:]!r} stderr={proc.stderr[-800:]!r}")
        self.assertEqual(rows[0].get("status"), "ok")
        value = rows[0].get("value", "")
        self.assertIn(":done", value)
        self.assertIn(":async", value)
        self.assertIn("1", value)
        self.assertIn("0", value)

    def test_eval_async_isolated_result(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        payload = json.dumps({"session": "default", "cmd": "exec", "code": '(eval:async "(+ 1 2)")'}) + "\n"
        proc = subprocess.run(
            [str(AURA_BIN), "--serve-async"],
            input=payload,
            text=True,
            capture_output=True,
            timeout=60,
            env=env,
        )
        rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(len(rows), 1, f"stdout={proc.stdout[-800:]!r} stderr={proc.stderr[-800:]!r}")
        self.assertEqual(rows[0].get("value"), '"3"')

    def test_snapshot_registry_is_bounded(self) -> None:
        # Issue #5513: every offload:snapshot kept a full workspace-source
        # copy for the life of the process. The handle table is now capped;
        # an unconsumed loop must fail with a clear error, not OOM.
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        program = "(begin (let loop ((i 0) (last 0)) (if (< i 70) (loop (+ i 1) (offload:snapshot)) last)))"
        proc = subprocess.run(
            [str(AURA_BIN)],
            input=program,
            text=True,
            capture_output=True,
            timeout=60,
            env=env,
        )
        out = proc.stdout + proc.stderr
        self.assertIn("too many unconsumed snapshots", out, out[-800:])

    def test_await_first_ticket_after_a_second_submit(self) -> None:
        # Issue #5514: the second submit bumps the fiber's wait generation,
        # so the first ticket's completion was dropped as stale and the
        # requester hung forever. Both awaits must return.
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "force-soa"
        program = (
            "(begin "
            "(define t1 (offload:submit (offload:snapshot) '(typecheck))) "
            "(define t2 (offload:submit (offload:snapshot) '(typecheck))) "
            "(define r1 (offload:await t1 :timeout-ms 30000)) "
            "(define r2 (offload:await t2 :timeout-ms 30000)) "
            '(list (hash-ref r1 "status") (hash-ref r2 "status")))'
        )
        payload = json.dumps({"session": "default", "cmd": "exec", "code": program}) + "\n"
        proc = subprocess.run(
            [str(AURA_BIN), "--serve-async"],
            input=payload,
            text=True,
            capture_output=True,
            timeout=60,
            env=env,
        )
        rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(len(rows), 1, f"stdout={proc.stdout[-800:]!r} stderr={proc.stderr[-800:]!r}")
        self.assertEqual(rows[0].get("status"), "ok", rows[0])
        self.assertIn(":done", rows[0].get("value", ""))

    def test_reused_worker_matches_fresh_snapshot(self) -> None:
        # Issue #5633: one pool thread reuses its CompilerService. An
        # ill-typed snapshot, a conflicting well-typed snapshot of the same
        # name, then the ill-typed snapshot again must match the first result.
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "force-soa"
        env["AURA_OFFLOAD_THREADS"] = "1"
        program = (
            "(begin "
            '(set-code "(: x String 1)") (eval-current) '
            "(define b1 (offload:await (offload:submit (offload:snapshot) "
            "'(typecheck) :class 'compile) :timeout-ms 30000)) "
            '(set-code "(: x Int 1)") (eval-current) '
            "(define a (offload:await (offload:submit (offload:snapshot) "
            "'(typecheck) :class 'compile) :timeout-ms 30000)) "
            '(set-code "(: x String 1)") (eval-current) '
            "(define b2 (offload:await (offload:submit (offload:snapshot) "
            "'(typecheck) :class 'compile) :timeout-ms 30000)) "
            '(define db (hash-ref b1 "diagnostics")) '
            '(define da (hash-ref a "diagnostics")) '
            '(define db2 (hash-ref b2 "diagnostics")) '
            '(list (if (equal? db db2) "DIAG_SAME" "DIAG_DIFF") '
            '(if (equal? db da) "A_SAME" "A_DIFF") '
            '(hash-ref b1 "status") db da))'
        )
        payload = json.dumps({"session": "default", "cmd": "exec", "code": program}) + "\n"
        proc = subprocess.run(
            [str(AURA_BIN), "--serve-async"],
            input=payload,
            text=True,
            capture_output=True,
            timeout=60,
            env=env,
        )
        rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(len(rows), 1, f"stdout={proc.stdout[-800:]!r} stderr={proc.stderr[-800:]!r}")
        self.assertEqual(rows[0].get("status"), "ok", rows[0])
        value = rows[0].get("value", "")
        self.assertIn("DIAG_SAME", value, value)
        self.assertIn("A_DIFF", value, value)
        self.assertIn(":done", value, value)


if __name__ == "__main__":
    unittest.main()
