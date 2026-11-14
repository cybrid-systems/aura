#!/usr/bin/env python3
"""Issue #4860: concurrent stats:get must not corrupt the hash registry.

g_hash_tables is a PROCESS global. Before the fix, a fiber worker that
minted a result hash (stats:get "gc-heap-stats" and friends) pushed into
that vector WITHOUT the evaluator's alloc_storage_lock_, so two workers
could reallocate it at the same time and glibc aborted the process:

    malloc(): unaligned tcache chunk detected
    === AURA CRASH: SIGABRT (signal 6) ===

The repro is the issue's own shape: 8+ fibers x 50 reads of a stats
panel, three processes at a time. Before the fix that aborted in roughly
70% of the runs; after it every run must exit 0 and print "end".
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

SCRIPT = """(define sf (map (lambda (i) (fiber:spawn (lambda ()
  (let loop ((k 0)) (if (< k {iters}) (begin (stats:get "{panel}") (loop (+ k 1))) k)))))
  (range 0 {fibers})))
(display (map fiber:join sf)) (newline)
(display "end") (newline)
"""

PANELS = ("gc-heap-stats", "closure:stats", "concurrency:stats")
FIBERS = 32
ITERS = 500
ROUNDS = 4


def _env() -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    return env


class TestIssue4860(unittest.TestCase):
    def test_concurrent_stats_reads_do_not_corrupt_the_heap(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            for i, panel in enumerate(PANELS):
                p = Path(tmp) / f"st_{i}.aura"
                p.write_text(
                    SCRIPT.format(fibers=FIBERS, iters=ITERS, panel=panel),
                    encoding="utf-8",
                )
                paths.append(p)

            def run(p: Path) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [str(AURA_BIN), "--load", str(p)],
                    capture_output=True,
                    text=True,
                    env=_env(),
                    timeout=180,
                )

            with ThreadPoolExecutor(max_workers=3) as pool:
                results = list(pool.map(run, paths * ROUNDS))

        for i, r in enumerate(results):
            self.assertEqual(
                0,
                r.returncode,
                f"run {i} exited {r.returncode} (heap corruption abort?)\n"
                f"stdout={r.stdout[-400:]}\nstderr={r.stderr[-400:]}",
            )
            self.assertIn("end", r.stdout, f"run {i} did not reach the end")


if __name__ == "__main__":
    unittest.main()
