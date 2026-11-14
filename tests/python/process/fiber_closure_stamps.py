#!/usr/bin/env python3
"""Regression for #4891: closure_slot_tenants_ grew 8 B per closure forever.

The tree-walker closure owner stamp is now written only on the face that
reads it (tenant_stamps_observable) and dropped with the closure (GC sweep /
closure:free!), so a mint-and-drop closure loop keeps the table flat.
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


def _run(source: str, extra_env: dict | None = None, timeout: float = 120.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    if extra_env:
        env.update(extra_env)
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
    finally:
        os.unlink(path)


def _field(out: str, key: str) -> int:
    m = re.search(rf"{key}=(-?\d+)", out)
    if not m:
        raise AssertionError(f"{key} missing in output: {out!r}")
    return int(m.group(1))


_STAMPS = """
(define (stamps) (let ((h (stats:get "gc-heap-stats")))
  (if (hash-has-key? h "closure-tenant-stamps") (hash-ref h "closure-tenant-stamps") -1)))
(define (mint-drop n)
  (let loop ((i 0) (acc 0))
    (if (< i n)
        (let ((f (lambda (x) (+ x i))))
          (when (= 0 (modulo i 500)) (gc-heap))
          (loop (+ i 1) (+ acc (f 1))))
        acc)))
"""


class TestClosureTenantStamps4891(unittest.TestCase):
    def test_off_face_writes_no_stamps(self) -> None:
        src = (
            _STAMPS
            + """
(mint-drop 4000)
(gc-heap)
(display (format "stamps=~a\\n" (stamps)))
"""
        )
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr[-2000:])
        # Before: no gauge and one 8 B slot per closure ever minted.
        self.assertEqual(_field(p.stdout, "stamps"), 0, p.stdout)

    def test_prod_face_stamps_are_reclaimed(self) -> None:
        src = (
            _STAMPS
            + """
(mint-drop 200) (gc-heap)
(define s0 (stamps))
;; live closures keep their stamp (the face is armed and still enforced)
(define keep (let loop ((i 0) (acc (list))) (if (< i 300) (loop (+ i 1) (cons (lambda () i) acc)) acc)))
(define s1 (stamps))
(mint-drop 6000) (gc-heap) (gc-heap)
(define s2 (stamps))
(display (format "s0=~a s1=~a s2=~a keep=~a\\n" s0 s1 s2 (length keep)))
"""
        )
        p = _run(src, {"AURA_SANDBOX": "restricted", "AURA_MULTI_TENANT": "1"})
        self.assertEqual(p.returncode, 0, p.stderr[-2000:])
        s0 = _field(p.stdout, "s0")
        s1 = _field(p.stdout, "s1")
        s2 = _field(p.stdout, "s2")
        self.assertGreaterEqual(s0, 0, p.stdout)
        self.assertGreaterEqual(s1 - s0, 300, p.stdout)
        # 6000 more closures minted and dropped: the stamp table must not
        # keep one entry each.
        self.assertLess(s2 - s1, 1000, p.stdout)


if __name__ == "__main__":
    unittest.main()
