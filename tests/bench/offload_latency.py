#!/usr/bin/env python3
"""Issue #4889: foreground latency while the offload pool is busy.

Measures eval:async round-trip p50/p99 on an idle pool, then with four
Compile-class typecheck jobs queued (weighted scheduling should keep the
Interactive path responsive). Prints both distributions and fails when the
p99 delta exceeds 50 ms (the issue's target is 20 ms; the CI bound leaves
headroom for a shared runner).
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SAMPLES = 25

PROGRAM = r"""
(begin
  (set-code "(require \"std/test\" all:)(test-suite \"basic\" (check= 3 (+ 1 2)))")
  (eval-current)
  (define (one)
    (let ((t0 (current-time-ms)))
      (eval:async "(+ 1 2)")
      (- (current-time-ms) t0)))
  (define *base* '())
  (define *i* 0)
  (while (lambda () (< *i* SAMPLES))
    (lambda () (set! *base* (cons (one) *base*)) (set! *i* (+ *i* 1))))
  (define *tickets* '())
  (define *j* 0)
  (while (lambda () (< *j* 4))
    (lambda ()
      (set! *tickets*
            (cons (offload:submit (offload:snapshot) '(typecheck) :class 'compile) *tickets*))
      (set! *j* (+ *j* 1))))
  (define *load* '())
  (define *k* 0)
  (while (lambda () (< *k* SAMPLES))
    (lambda () (set! *load* (cons (one) *load*)) (set! *k* (+ *k* 1))))
  (define (drain xs)
    (if (null? xs) 0 (begin (offload:await (car xs)) (drain (cdr xs)))))
  (drain *tickets*)
  (list (reverse *base*) (reverse *load*)))
""".replace("SAMPLES", str(SAMPLES))


def pct(values: list[int], q: float) -> int:
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, int(round(q * (len(ordered) - 1)))))
    return ordered[idx]


def main() -> int:
    aura = ROOT / os.environ.get("AURA_BUILD_DIR", "build") / "aura"
    if not aura.exists():
        print(f"skip: {aura} not built")
        return 0
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "force-soa"
    payload = json.dumps({"session": "default", "cmd": "exec", "code": PROGRAM}) + "\n"
    proc = subprocess.run(
        [str(aura), "--serve-async"], input=payload, text=True, capture_output=True, timeout=600, env=env
    )
    rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith("{")]
    if len(rows) != 1 or rows[0].get("status") != "ok":
        print(f"FAIL: {proc.stdout[-800:]!r} {proc.stderr[-800:]!r}")
        return 1
    nums = [int(m) for m in re.findall(r"\d+", rows[0]["value"])]
    base, load = nums[:SAMPLES], nums[SAMPLES : 2 * SAMPLES]
    print(f"idle  p50={pct(base, 0.5)}ms p99={pct(base, 0.99)}ms")
    print(f"busy  p50={pct(load, 0.5)}ms p99={pct(load, 0.99)}ms")
    delta = pct(load, 0.99) - pct(base, 0.99)
    print(f"p99 delta = {delta}ms (target < 20ms, CI bound < 50ms)")
    if delta >= 50:
        print("FAIL: interactive latency degraded beyond the bound")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
