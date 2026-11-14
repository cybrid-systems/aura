#!/usr/bin/env python3
"""Regression for #5099 and #5100: finished fibers do not fill the EnvFrame arena.

Spawning and joining a fiber used to leave its capture frames allocated
until a full-heap sweep, and every sweep recounted the whole dead set. After
20 000 fibers the live-frame metric sat near 78 000 and the reclamation
counter grew by hundreds of frames per fiber. Joining must make those frames
reclaimable without an explicit (gc), and the counter must grow by O(1)
frames per fiber.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

# Sequential spawn+join. 20 000 is the point where the old accounting already
# reported ~78 000 "live" frames. Each fiber captures three closures.
PROG = r"""
(define (body i)
  (let ((f (lambda (x) (+ x i)))
        (g (lambda (y) (f y)))
        (h (lambda (z) (g z))))
    (h 1)))
(define (go n)
  (let loop ((i 0))
    (if (< i n)
        (begin
          (fiber:join (fiber:spawn (lambda () (body i))))
          (loop (+ i 1)))
        i)))
(define n 20000)
(define got (go n))
(define c (hash-ref (engine:metrics) "compiler" (hash)))
(display (format "spawned=~a live=~a reclaimed=~a slots=~a\n"
                 got
                 (hash-ref c "env_frames_live_total" -1)
                 (hash-ref c "env_frames_reclaimed_total" -1)
                 (hash-ref c "env_frames_size_total" -1)))
"""


def run_prog(src: str) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    env.pop("AURA_PRESET", None)
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=180,
            stdin=subprocess.DEVNULL,
        )
    finally:
        os.unlink(path)


class EnvFrameFiberChurn(unittest.TestCase):
    def test_joined_fibers_do_not_fill_the_arena(self) -> None:
        proc = run_prog(PROG)
        out = (proc.stdout or "") + (proc.stderr or "")
        self.assertEqual(proc.returncode, 0, out)
        self.assertNotIn("EnvFrame arena exhausted", out)
        m = re.search(
            r"spawned=(\d+) live=(-?\d+) reclaimed=(-?\d+) slots=(-?\d+)",
            proc.stdout or "",
        )
        self.assertIsNotNone(m, proc.stdout)
        spawned = int(m.group(1))
        live = int(m.group(2))
        reclaimed = int(m.group(3))
        slots = int(m.group(4))
        self.assertEqual(spawned, 20000)
        # The true live set is the top-level program, not one frame per fiber.
        self.assertLess(live, 500, f"live={live} slots={slots}")
        self.assertLess(slots, 5000, f"arena grew to {slots}")
        per = reclaimed / spawned
        # Old path: ~318 reclamations per fiber. A handful of capture frames
        # per fiber, each counted once, stays under this bound.
        self.assertLess(per, 8.0, f"reclaimed/fiber={per:.2f} (reclaimed={reclaimed})")

    def test_exhaustion_diagnostic_names_allocated_and_live(self) -> None:
        header = (ROOT / "src" / "compiler" / "env_frame_arena.hh").read_text()
        self.assertIn("allocated capture frames", header)
        self.assertIn(" live,", header)
        self.assertIn("kMaxSlots", header)


if __name__ == "__main__":
    unittest.main()
