#!/usr/bin/env python3
"""Issue #4889: offload submit/await soak (RSS bounded, queue drains).

Runs the offload loop on the real --serve-async pool: every round takes a
snapshot, submits a typecheck job, awaits it, and drops the ticket. RSS is
read inside the script (/proc/self/status) after a warmup window and at the
end, so the measurement does not depend on host polling.
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

ROUNDS = int(os.environ.get("AURA_OFFLOAD_SOAK_ROUNDS", "200"))
WARMUP = max(5, ROUNDS // 4)

PROGRAM = r"""
(begin
  (set-code "(require \"std/test\" all:)(test-suite \"basic\" (check= 3 (+ 1 2)))")
  (eval-current)
  (define (rss-kb)
    (let ((s (read-file "/proc/self/status")))
      (let ((n (string-length s)) (i 0) (found -1))
        (while (lambda () (and (= found -1) (< i (- n 6))))
          (lambda ()
            (if (equal? (substring s i (+ i 6)) "VmRSS:") (set! found i) 0)
            (set! i (+ i 1))))
        (if (= found -1) 0
            (let ((k (+ found 6)) (v 0) (done #f))
              (while (lambda () (and (< k n) (or (= (string-ref s k) 32) (= (string-ref s k) 9))))
                (lambda () (set! k (+ k 1))))
              (while (lambda () (and (not done) (< k n)))
                (lambda ()
                  (let ((c (string-ref s k)))
                    (if (and (>= c 48) (<= c 57))
                        (begin (set! v (+ (* v 10) (- c 48))) (set! k (+ k 1)))
                        (set! done #t)))))
              v)))))
  (define *i* 0)
  (while (lambda () (< *i* WARMUP))
    (lambda ()
      (offload:await (offload:submit (offload:snapshot) '(typecheck) :class 'interactive))
      (set! *i* (+ *i* 1))))
  (define *base* (rss-kb))
  (while (lambda () (< *i* ROUNDS))
    (lambda ()
      (offload:await (offload:submit (offload:snapshot) '(typecheck) :class 'interactive))
      (set! *i* (+ *i* 1))))
  (string-append (number->string *base*) ","
                 (number->string (rss-kb)) ","
                 (stats:get "orch:metrics")))
""".replace("ROUNDS", str(ROUNDS)).replace("WARMUP", str(WARMUP))


class TestOffloadSoak(unittest.TestCase):
    def test_submit_await_soak(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "force-soa"
        payload = json.dumps({"session": "default", "cmd": "exec", "code": PROGRAM}) + "\n"
        proc = subprocess.run(
            [str(AURA_BIN), "--serve-async"],
            input=payload,
            text=True,
            capture_output=True,
            timeout=900,
            env=env,
        )
        rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(len(rows), 1, f"stdout={proc.stdout[-800:]!r} stderr={proc.stderr[-800:]!r}")
        self.assertEqual(rows[0].get("status"), "ok", rows[0])

        raw = rows[0].get("value", "")
        if raw.startswith('"') and raw.endswith('"'):
            raw = raw[1:-1]
        base_str, final_str, metrics_json = raw.split(",", 2)
        base = int(base_str)
        final = int(final_str)
        offload = json.loads(metrics_json)["offload"]
        self.assertEqual(offload["queue_depth"], 0)
        self.assertGreaterEqual(offload["completed_done_total"], ROUNDS)
        self.assertEqual(offload["completed_failed_total"], 0)

        self.assertGreater(base, 0, "could not read VmRSS")
        self.assertLess(
            final - base,
            max(1024 * 20, int(base * 0.10)),
            f"RSS grew {base} -> {final} kB across {ROUNDS} offload rounds",
        )


PROCESS_THEME = "offload-soak"
PROCESS_TIER = "l2"
PROCESS_TIMEOUT = 1000
PARTS = ()
CASES = (
    (
        4889,
        "test_submit_await_soak",
        "TestOffloadSoak.test_submit_await_soak",
    ),
)


def load_tests(loader, tests, pattern):
    import importlib
    import sys

    suite = unittest.TestSuite()
    for _issue, _title, spec in CASES:
        bits = spec.split(".")
        if len(bits) == 3:
            mod = importlib.import_module(bits[0])
            suite.addTest(getattr(mod, bits[1])(bits[2]))
        else:
            suite.addTest(getattr(sys.modules[__name__], bits[0])(bits[1]))
    return suite


if __name__ == "__main__":
    unittest.main()
