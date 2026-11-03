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

ROOT = Path(__file__).resolve().parents[2]
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


if __name__ == "__main__":
    unittest.main()
