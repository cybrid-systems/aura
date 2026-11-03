#!/usr/bin/env python3
"""Issue #4884: offload pool metrics on (stats:get "orch:metrics") and /metrics."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
import unittest
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

PROGRAM = r"""
(begin
  (set-code "(require \"std/test\" all:)(test-suite \"basic\" (check= 3 (+ 1 2)))")
  (eval-current)
  (offload:await (offload:submit (offload:snapshot) '(typecheck) :class 'interactive))
  (stats:get "orch:metrics"))
"""


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class TestOffloadMetrics(unittest.TestCase):
    def test_metrics_faces_expose_offload(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        port = free_port()
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "force-soa"
        payload = json.dumps({"session": "default", "cmd": "exec", "code": PROGRAM}) + "\n"
        proc = subprocess.Popen(
            [str(AURA_BIN), "--serve-async", "--health-port", str(port)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        try:
            assert proc.stdin is not None
            proc.stdin.write(payload)
            proc.stdin.flush()

            body = ""
            deadline = time.time() + 30
            while time.time() < deadline:
                try:
                    with urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=2) as r:
                        body = r.read().decode()
                    break
                except OSError:
                    time.sleep(0.2)
            self.assertIn("aura_offload_submitted_total", body, body[-500:])
            self.assertIn('aura_offload_completed_total{status="done"}', body, body[-500:])
            self.assertIn("aura_offload_queue_depth 0", body, body[-500:])
            self.assertIn("aura_offload_threads", body, body[-500:])
            self.assertIn("aura_offload_exec_us_bucket", body, body[-500:])

            assert proc.stdin is not None
            proc.stdin.close()
            out, err = proc.communicate(timeout=60)
        finally:
            if proc.poll() is None:
                proc.kill()

        rows = [json.loads(line) for line in out.splitlines() if line.startswith("{")]
        self.assertEqual(len(rows), 1, f"stdout={out[-800:]!r} stderr={err[-800:]!r}")
        self.assertEqual(rows[0].get("status"), "ok")
        raw = rows[0]["value"]
        # The serve face formats a string value with surrounding quotes but
        # does not JSON-escape the payload; strip them and parse the JSON text.
        if raw.startswith('"') and raw.endswith('"'):
            raw = raw[1:-1]
        metrics = json.loads(raw)
        offload = metrics.get("offload")
        self.assertIsNotNone(offload, metrics)
        self.assertGreaterEqual(offload["submitted_total"], 1)
        self.assertGreaterEqual(offload["completed_done_total"], 1)
        self.assertEqual(offload["queue_depth"], 0)
        self.assertGreaterEqual(offload["threads"], 1)
        self.assertIn("by_class", offload)
        self.assertIn("exec_us", offload)


if __name__ == "__main__":
    unittest.main()
