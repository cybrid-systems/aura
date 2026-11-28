#!/usr/bin/env python3
"""File-face split and top-level define lookup stay linear in file size.

Issue #5638: `aura file.aura` used to recount newlines from byte 0 on every
open paren. Issue #5639: each new top-level define reverse-scanned every
name already bound. Both are ratio gates against the same binary, not
absolute wall clocks.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def _run(source: str, *, load: bool, timeout: float) -> tuple[subprocess.CompletedProcess[str], float]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    env["AURA_PATH"] = str(ROOT / "lib")
    cmd = [str(AURA_BIN), "--load", path] if load else [str(AURA_BIN), path]
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)
    finally:
        os.unlink(path)
    return proc, time.perf_counter() - t0


def _best(source: str, *, load: bool, timeout: float, runs: int = 3) -> tuple[subprocess.CompletedProcess[str], float]:
    best_proc: subprocess.CompletedProcess[str] | None = None
    best = None
    for _ in range(runs):
        proc, wall = _run(source, load=load, timeout=timeout)
        if best is None or wall < best:
            best = wall
            best_proc = proc
    assert best_proc is not None and best is not None
    return best_proc, best


class TestDefineScale(unittest.TestCase):
    def test_redefine_same_name_ten_thousand(self) -> None:
        # Last define wins, including through a closure captured before the
        # redefines, and set! writes that cell.
        lines = ["(define (read-rebound) rebound)", "(define rebound 0)"]
        lines.extend(f"(define rebound {i})" for i in range(1, 10000))
        lines.append("(set! rebound (+ rebound 1))")
        lines.append("(display (read-rebound))")
        lines.append("(newline)")
        proc, _wall = _run("\n".join(lines) + "\n", load=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), "10000")

    def test_distinct_defines_scale_linearly(self) -> None:
        def program(n: int) -> str:
            lines = [f"(define v{i} {i})" for i in range(n)]
            mid = n // 2
            last = n - 1
            lines.append(f'(display v0) (display " ") (display v{mid}) (display " ") (display v{last})')
            lines.append("(newline)")
            return "\n".join(lines) + "\n"

        small_n, large_n = 20000, 50000
        small, t_small = _best(program(small_n), load=True, timeout=60)
        large, t_large = _best(program(large_n), load=True, timeout=90)
        self.assertEqual(small.returncode, 0, small.stderr)
        self.assertEqual(large.returncode, 0, large.stderr)
        self.assertEqual(small.stdout.strip(), f"0 {small_n // 2} {small_n - 1}")
        self.assertEqual(large.stdout.strip(), f"0 {large_n // 2} {large_n - 1}")
        # 2.5× names. Quadratic was ~4.8×. Linear stays under 3×.
        self.assertGreater(t_small, 0.0)
        self.assertLessEqual(t_large / t_small, 3.0, f"walls {t_small:.3f}s -> {t_large:.3f}s")

    def test_file_face_matches_load_within_five(self) -> None:
        n = 5000
        lines = [f"(define (f{i} x) (let ((y x)) (+ y {i})))" for i in range(n)]
        lines.append(f"(display (f{n - 1} 1))")
        lines.append("(newline)")
        source = "\n".join(lines) + "\n"
        loaded, t_load = _best(source, load=True, timeout=60)
        filed, t_file = _best(source, load=False, timeout=60)
        self.assertEqual(loaded.returncode, 0, loaded.stderr)
        self.assertEqual(filed.returncode, 0, filed.stderr)
        self.assertEqual(loaded.stdout, filed.stdout)
        self.assertEqual(loaded.stdout.strip(), str(n))
        self.assertGreater(t_load, 0.0)
        self.assertLessEqual(t_file / t_load, 5.0, f"file {t_file:.3f}s load {t_load:.3f}s")
