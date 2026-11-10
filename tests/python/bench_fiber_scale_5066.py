"""#5066: print the CLI fiber scaling curve. Not a pass/fail gate.

Usage (from the repo root, aura already built):
  AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 python3 tests/python/bench_fiber_scale_5066.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))
from _aura_harness import AURA_BIN  # noqa: E402

WORK = int(os.environ.get("FIBER_WORK", "150000"))
NS = [1, 2, 4, 8, 16, 32]
SRC = """
(define N {n})
(define WORK {work})
(define (body)
  (let loop ((i 0) (a 0))
    (if (>= i WORK) a
      (loop (+ i 1) (+ a (modulo i 7))))))
(define (spawn-n n)
  (let loop ((i 0) (ids '()))
    (if (>= i n) ids
      (loop (+ i 1) (cons (fiber:spawn (lambda () (body))) ids)))))
(define t0 (monotonic-ms))
(define ids (spawn-n N))
(define (join-all xs acc)
  (if (null? xs) acc
    (join-all (cdr xs) (+ acc (fiber:join (car xs))))))
(define total (join-all ids 0))
(define dt (- (monotonic-ms) t0))
(display "n=") (display N)
(display " wall_ms=") (display dt)
(display " checksum=") (display total)
(newline)
"""


def main() -> None:
    env = os.environ.copy()
    env.setdefault("AURA_SANDBOX", "off")
    env.setdefault("AURA_PIPELINE_STRICT", "0")
    one = None
    for n in NS:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fiber.aura"
            path.write_text(SRC.format(n=n, work=WORK))
            r = subprocess.run(
                [str(AURA_BIN), "--load", str(path)],
                capture_output=True,
                text=True,
                timeout=120,
                env=env,
            )
        print(r.stdout.strip() or r.stderr.strip())
        if r.returncode != 0:
            sys.exit(r.returncode)
        wall = int(r.stdout.split("wall_ms=")[1].split()[0])
        if n == 1:
            one = wall
        elif one:
            speedup = (one * n) / wall
            print(f"  speedup_vs_1={speedup:.2f}x")


if __name__ == "__main__":
    main()
