#!/usr/bin/env python3
"""Regression test for issue #4854 residual: per-fiber string slabs leaked.

Issue #4714 Row 2 gave every scheduler fiber a string slab: the first string
a body mints claims a batch of 512+ real string-heap slots, and the reclaim
skips claimed slots until the batch is closed. Only fiber:spawn's completion
ever closed one (publish_fiber_slab); orch agent / scope bodies -- and every
other scheduler body -- left the batch claimed and its arena in the table
for the life of the process. An erroring agent body (the error message is a
minted string) therefore pinned ~22 KB per agent; the soak grew ~20 MB per
1000 agents whenever the round also called a stdlib closure (foldl,
member?) around the join.

The fiber now runs a body-exit hook that drops the slab of every Evaluator
it minted into, so these loops are flat.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

ITERS = 1500
WARMUP = 300

_SRC = r"""
(define (rss-kb)
  (let* ((s (read-file "/proc/self/status")) (i (string-index s "VmRSS:")))
    (let loop ((j (+ i 6)) (acc 0) (seen #f))
      (let ((c (string-ref s j)))
        (cond ((char-numeric? c) (loop (+ j 1) (+ (* acc 10) (- (char->integer c) 48)) #t))
              (seen acc)
              (else (loop (+ j 1) acc seen)))))))
(define (nm r) (format "slab~a" r))
(define (once r)
  __BODY__
  (when (= 0 (modulo r 5)) (gc-heap)))
(define k1 0)
(let loop ((r 0))
  (when (< r __ITERS__)
    (once r)
    (when (= r __WARMUP__) (set! k1 (rss-kb)))
    (loop (+ r 1))))
(gc-heap)
(display (format "GROWTH ~a ~a\n" k1 (rss-kb)))
"""

_PROBES = {
    # The soak shape: an erroring agent joined inside a stdlib fold.
    "agent-error-foldl": """(orch:spawn-agent (nm r) (lambda () (error "planned")))
  (foldl (lambda (a k) (hash-ref (orch:agent-join (nm r) :timeout-ms 5000) "ok")) 0 (list 1))""",
    "agent-error-member": """(orch:spawn-agent (nm r) (lambda () (error "planned")))
  (orch:agent-join (nm r) :timeout-ms 5000)
  (member? 1 (list 1))""",
    # A string-minting (non-erroring) body.
    "agent-string-foldl": """(orch:spawn-agent (nm r) (lambda () (string-append "s" (number->string r))))
  (orch:agent-join (nm r) :timeout-ms 5000)
  (foldl + 0 (list 1))""",
    "scope-error-foldl": """(orch:scope-spawn (nm r) (lambda () (error "planned")))
  (orch:scope-join-all :timeout-ms 5000)
  (foldl + 0 (list 1))""",
}

# Pre-fix: ~22 KB/iteration (~26 MB over the measured window). Post-fix the
# allocator high-water is well under 1 MB.
LIMIT_KB = 4 * 1024


def _run(body: str) -> tuple[int, int]:
    src = _SRC.replace("__BODY__", body).replace("__ITERS__", str(ITERS)).replace("__WARMUP__", str(WARMUP))
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(src)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    try:
        proc = subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
            stdin=subprocess.DEVNULL,
        )
    finally:
        os.unlink(path)
    m = re.search(r"GROWTH (\d+) (\d+)", proc.stdout)
    if proc.returncode != 0 or not m:
        raise AssertionError(f"probe failed rc={proc.returncode}\n{proc.stdout[-1500:]}\n{proc.stderr[-1500:]}")
    return int(m.group(1)), int(m.group(2))


class TestIssue4854FiberSlabRelease(unittest.TestCase):
    def test_agent_and_scope_bodies_release_their_slab(self) -> None:
        for name, body in _PROBES.items():
            with self.subTest(probe=name):
                k0, k1 = _run(body)
                self.assertLess(
                    k1 - k0,
                    LIMIT_KB,
                    f"#4854 {name}: RSS grew {k1 - k0} KB over {ITERS - WARMUP} "
                    f"iterations ({k0} -> {k1} KB): the body's per-fiber string "
                    "slab is not released at fiber exit again.",
                )


if __name__ == "__main__":
    unittest.main()
