#!/usr/bin/env python3
r"""Regression for #5057: EnvFrame arena exhaustion is catchable and file mode reclaims dead frames.

Three defects, one report:

  * production exhaustion called abort(): SIGABRT, no error value, no
    catchable exit. A legitimate program holding >1,048,576 live closures was
    simply killed.
  * file mode (reclaim_file_mode_unreachable) rooted EVERY env frame, so a
    dead capture frame kept its slot until compact/truncate. The arena budget
    was cumulative allocation instead of live frames, and there was no
    watermark/hit-miss/reclaimed surface to see it coming.
  * the tree-walker sweep already had the fixup; the file path did not.

The test drives the real CLI on both faces (AURA_SANDBOX=off file mode and
the --serve-async backend), so it covers the surface the issue was filed from.
"""

from __future__ import annotations

import json
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

# The issue's 18-line reproducer: 4 fibers x 300 x 1000 = 1,200,000 live
# closures, well past kMaxSlots (1,048,576).
REPRO = """(define keep (list))
(define (worker T C)
  (let loop ((t 0))
    (if (< t T)
        (begin
          (let inner ((c 0))
            (if (< c C)
                (begin
                  (set! keep (cons (lambda () c) keep))
                  (inner (+ c 1)))
                #f))
          (loop (+ t 1)))
        #f)))
(define (main)
  (let loop ((i 0))
    (if (< i 4)
        (begin
          (fiber:join (fiber:spawn (lambda () (worker 300 1000))))
          (loop (+ i 1)))
        (length keep))))
(display (format "kept ~a\\n" (main)))
"""

# Same allocation, but the exhaustion is observed by an Aura handler: AC1's
# "the program gets a handleable error".
REPRO_CATCH = REPRO.replace(
    '(display (format "kept ~a\\n" (main)))',
    '(display (try (main) (catch (e) (format "caught: ~a\\n" e))))',
)

# AC3: allocate 300k capture frames, keep only 100. A reclaiming file mode
# keeps the slot watermark O(M); the pre-fix path grew it to O(N).
CHURN = """(define (stat k) (hash-ref (hash-ref (engine:metrics) "compiler") k 0))
(define (dump tag)
  (display (format "~a slots=~a live=~a hw=~a hits=~a misses=~a reclaimed=~a\\n"
                   tag (stat "env_frames_size_total") (stat "env_frames_live_total")
                   (stat "env_frames_high_water") (stat "env_frame_reuse_hits_total")
                   (stat "env_frame_reuse_misses_total") (stat "env_frames_reclaimed_total"))))
(define (churn n)
  (let loop ((i 0))
    (if (< i n)
        (begin (lambda () i) (loop (+ i 1)))
        #f)))
(define keep (list))
(define (keepm m)
  (let loop ((i 0))
    (if (< i m)
        (begin (set! keep (cons (lambda () i) keep)) (loop (+ i 1)))
        (length keep))))
(keepm 100)
(dump "start")
(churn 200000)
(dump "after-200k")
(display (format "kept=~a\\n" (length keep)))
"""


def base_env() -> dict[str, str]:
    env = dict(os.environ)
    for k in ("AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(k, None)
    env["AURA_SANDBOX"] = "off"
    return env


def run_file(src: str, *, timeout: int = 900) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=base_env(),
            timeout=timeout,
            stdin=subprocess.DEVNULL,
        )
    finally:
        os.unlink(path)


def run_serve(src: str, *, timeout: int = 900) -> tuple[subprocess.CompletedProcess[str], str]:
    env = base_env()
    # The Soft serve-async face refuses the tree-walker fallback unless the
    # pipeline override is explicit (#2213); the issue names this face.
    env["AURA_PIPELINE_STRICT"] = "0"
    request = json.dumps({"cmd": "exec", "code": src}) + "\n"
    proc = subprocess.run(
        [str(AURA_BIN), "--serve-async"],
        input=request,
        capture_output=True,
        text=True,
        env=env,
        timeout=timeout,
    )
    return proc, env["AURA_PIPELINE_STRICT"]


def parse_dump(text: str, tag: str) -> dict[str, int]:
    m = re.search(rf"^{re.escape(tag)} (.*)$", text, re.MULTILINE)
    if not m:
        raise AssertionError(f"missing {tag} dump in:\n{text}")
    return {k: int(v) for k, v in re.findall(r"([a-z]+)=(-?\d+)", m.group(1))}


class ExhaustionIsCatchable(unittest.TestCase):
    def test_file_mode_reports_an_error_instead_of_sigabrt(self) -> None:
        proc = run_file(REPRO)
        out = (proc.stdout or "") + (proc.stderr or "")
        self.assertNotIn("SIGABRT", out)
        self.assertNotIn("AURA CRASH", out)
        self.assertNotEqual(proc.returncode, -6, "abort() must not be the exit")
        self.assertNotEqual(proc.returncode, 134, "SIGABRT must not be the exit")
        self.assertIn("EnvFrame arena exhausted", out)
        self.assertNotEqual(proc.returncode, 0, "a top-level exhaustion must fail the run")

    def test_an_aura_handler_can_catch_it(self) -> None:
        proc = run_file(REPRO_CATCH)
        out = (proc.stdout or "") + (proc.stderr or "")
        self.assertNotIn("SIGABRT", out)
        self.assertEqual(proc.returncode, 0, out)
        self.assertIn("caught:", proc.stdout or "")
        self.assertIn("EnvFrame arena exhausted", proc.stdout or "")

    def test_serve_async_backend_survives(self) -> None:
        proc, _ = run_serve(REPRO)
        out = (proc.stdout or "") + (proc.stderr or "")
        self.assertNotIn("Aborted", out)
        self.assertNotIn("SIGABRT", out)
        self.assertEqual(proc.returncode, 0, out)
        self.assertIn("EnvFrame arena exhausted", out)

    def test_serve_async_handler_catches_it(self) -> None:
        proc, _ = run_serve(REPRO_CATCH)
        out = (proc.stdout or "") + (proc.stderr or "")
        self.assertNotIn("Aborted", out)
        self.assertEqual(proc.returncode, 0, out)
        replies = [line for line in (proc.stdout or "").splitlines() if line.startswith("{")]
        self.assertTrue(replies, out)
        reply = json.loads(replies[-1])
        self.assertEqual(reply.get("status"), "ok", reply)
        self.assertIn("caught:", str(reply.get("display", "")), reply)
        self.assertIn("EnvFrame arena exhausted", str(reply.get("display", "")), reply)


class FileModeReclaimsDeadFrames(unittest.TestCase):
    def test_slot_watermark_stays_proportional_to_live_frames(self) -> None:
        proc = run_file(CHURN)
        self.assertEqual(proc.returncode, 0, (proc.stdout or "") + (proc.stderr or ""))
        start = parse_dump(proc.stdout, "start")
        after = parse_dump(proc.stdout, "after-200k")
        self.assertIn("kept=100", proc.stdout)
        # 200k allocations, 100 live closures: the arena must not grow with N.
        self.assertLess(after["slots"], 5000, after)
        self.assertGreaterEqual(after["reclaimed"], 100000, after)
        self.assertGreater(after["hits"], 100000, after)
        self.assertGreaterEqual(start["live"], 100)
        # AC4: the pressure surface is present before the cap is reached.
        for key in ("slots", "live", "hw", "hits", "misses", "reclaimed"):
            self.assertIn(key, after)


PROCESS_THEME = "env-frame"
PROCESS_TIER = "l2"
PROCESS_TIMEOUT = 1000
PARTS = ()
CASES = (
    (
        5057,
        "test_file_mode_reports_an_error_instead_of_sigabrt",
        "ExhaustionIsCatchable.test_file_mode_reports_an_error_instead_of_sigabrt",
    ),
    (
        5057,
        "test_an_aura_handler_can_catch_it",
        "ExhaustionIsCatchable.test_an_aura_handler_can_catch_it",
    ),
    (
        5057,
        "test_serve_async_backend_survives",
        "ExhaustionIsCatchable.test_serve_async_backend_survives",
    ),
    (
        5057,
        "test_serve_async_handler_catches_it",
        "ExhaustionIsCatchable.test_serve_async_handler_catches_it",
    ),
    (
        5057,
        "test_slot_watermark_stays_proportional_to_live_frames",
        "FileModeReclaimsDeadFrames.test_slot_watermark_stays_proportional_to_live_frames",
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
    unittest.main(verbosity=2)
