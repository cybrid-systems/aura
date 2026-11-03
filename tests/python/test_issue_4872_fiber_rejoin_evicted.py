#!/usr/bin/env python3
"""Regression test for issue #4872: re-joining an evicted fiber hung.

#4854 bounded the #4839 repeated-join cache to a 1024-entry FIFO. A second
(fiber:join f) after more than 1024 other joins found f in neither the
result registry (the first join erased it) nor the cache (evicted), fell
into the wait loops and polled for a result that can never be published:
the CLI path blocked for its 200 s deadline, a scheduler joiner forever. A
fid that fiber:spawn never returned hung the same way.

fiber:spawn registers the result slot before it returns the fid, so a fid
that is in neither table answers void at once (the documented "older
repeat joins return void" contract). Joins that are still cached keep
returning the value / re-raising the error.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

# Generous: pre-fix every case below blocked for >= 200 s (or forever).
DEADLINE_S = 60.0
FAST_S = 20.0


def _run(source: str) -> tuple[subprocess.CompletedProcess[str], float]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=DEADLINE_S,
            stdin=subprocess.DEVNULL,
        )
    finally:
        os.unlink(path)
    return proc, time.monotonic() - t0


class TestIssue4872RejoinEvicted(unittest.TestCase):
    def _check(self, source: str, expect: list[str]) -> None:
        try:
            proc, elapsed = _run(source)
        except subprocess.TimeoutExpired:
            self.fail(f"#4872: fiber:join did not return within {DEADLINE_S:.0f} s (hang)")
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        for needle in expect:
            self.assertIn(needle, proc.stdout, proc.stdout[-2000:])
        self.assertLess(elapsed, FAST_S, f"#4872: took {elapsed:.1f} s:\n{proc.stdout[-1000:]}")

    def test_rejoin_after_cache_eviction_top_level(self) -> None:
        self._check(
            """
(define f (fiber:spawn (lambda () 42)))
(display (list 'first (fiber:join f))) (newline)
(display (list 'cached (fiber:join f))) (newline)
(let loop ((i 0)) (when (< i 1100) (fiber:join (fiber:spawn (lambda () i))) (loop (+ i 1))))
(define v (fiber:join f))
(display (list 'evicted (void? v))) (newline)
(display (list 'again (void? (fiber:join f)))) (newline)
""",
            ["(first 42)", "(cached 42)", "(evicted #t)", "(again #t)"],
        )

    def test_rejoin_after_cache_eviction_inside_fiber(self) -> None:
        # The joiner is itself a scheduler fiber (the wait path that parked
        # forever pre-fix).
        self._check(
            """
(define f (fiber:spawn (lambda () 7)))
(fiber:join f)
(define g
  (fiber:spawn
    (lambda ()
      (let loop ((i 0)) (when (< i 1100) (fiber:join (fiber:spawn (lambda () i))) (loop (+ i 1))))
      (void? (fiber:join f)))))
(display (list 'nested (fiber:join g))) (newline)
""",
            ["(nested #t)"],
        )

    def test_join_unknown_fid_returns(self) -> None:
        self._check(
            """
(display (list 'never (void? (fiber:join 987654321)))) (newline)
(define f (fiber:spawn (lambda () 5)))
(display (list 'live (fiber:join f))) (newline)
""",
            ["(never #t)", "(live 5)"],
        )

    def test_cached_error_still_reraises(self) -> None:
        # #4839 contract kept: within the cache window a second join of a
        # raising fiber re-raises the same error.
        self._check(
            """
(define f (fiber:spawn (lambda () (error "boom4872"))))
(display (list 'one (try (fiber:join f) (catch (e) 'caught)))) (newline)
(display (list 'two (try (fiber:join f) (catch (e) 'caught)))) (newline)
""",
            ["(one caught)", "(two caught)"],
        )


if __name__ == "__main__":
    unittest.main()
