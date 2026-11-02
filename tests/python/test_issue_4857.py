#!/usr/bin/env python3
"""Issue #4857: define-hygienic-macro calls must stay linear.

The tree-walker expands a macro call every time the call is evaluated, and
each hygienic expansion appends nodes to the flat and restamps it. Before
#4857 the eval_ir face handed every flat containing a MacroDef to the
walker, so ONE textual macro call inside a hot loop re-expanded the macro
per iteration: 2k calls took ~2.1s and 20k calls ~55s on a Release build.
The static macro_expand_all pass expands the call once at load time; the
loop body then runs at hand-written speed.

The test also covers a flat that defines BOTH a legacy defmacro and a
hygienic macro: the legacy MacroDef must not drag the hygienic call sites
onto the runtime walker (eval() routing, #4857).

Run:
    python3 tests/python/test_issue_4857.py
Set AURA_BUILD_DIR when using a non-default build directory.
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

_HYGIENIC = """
(define-hygienic-macro (or2-h a b) (let ((t a)) (if t t b)))
(define (run n)
  (let ((t0 (current-time-ms)))
    (let loop ((i 0) (acc 0))
      (if (< i n)
          (loop (+ i 1) (+ acc (or2-h #f i)))
          (list n (- (current-time-ms) t0) 'ms acc)))))
(display (run 2000)) (newline)
(display (run 20000)) (newline)
"""

_MIXED = (
    """
(defmacro (unused-legacy x) x)
"""
    + _HYGIENIC
)

# 2k calls are ~20ms after the fix; 20k ~200ms. The pre-#4857 evals were
# ~2.1s and ~55s. Generous ceilings keep slow/loaded CI meaningful while
# still failing the super-linear shape (10x calls ~25x+ time).
_LINEAR_RATIO = 20
_CEIL_2K_MS = 5000
_CEIL_20K_MS = 15000


def _env() -> dict[str, str]:
    env = dict(os.environ)
    # Macros are a Soft/dev construct: the production sandbox denies
    # MacroSelfEvo. The issue repro uses the same permissive face.
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    return env


def _run(args: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=cwd,
        capture_output=True,
        text=True,
        env=_env(),
        timeout=90,
    )


def _samples(stdout: str) -> dict[int, int]:
    """Parse (N <ms> ms <acc>) lines into {N: ms}."""
    out: dict[int, int] = {}
    for line in stdout.splitlines():
        m = re.match(r"^\((\d+) (\d+) ms (\d+)\)$", line.strip())
        if m:
            out[int(m.group(1))] = int(m.group(2))
    return out


class TestIssue4857(unittest.TestCase):
    def _assert_linear(self, face: str, stdout: str) -> None:
        t = _samples(stdout)
        self.assertIn(2000, t, f"{face}: missing 2k sample in {stdout!r}")
        self.assertIn(20000, t, f"{face}: missing 20k sample in {stdout!r}")
        t2k, t20k = t[2000], t[20000]
        self.assertLess(t2k, _CEIL_2K_MS, f"{face}: 2k calls took {t2k}ms")
        self.assertLess(t20k, _CEIL_20K_MS, f"{face}: 20k calls took {t20k}ms")
        # 10x the calls may cost at most 20x the time (+2s of fixed noise).
        ceiling = max(2000, _LINEAR_RATIO * max(t2k, 1))
        self.assertLessEqual(
            t20k,
            ceiling,
            f"{face}: macro call cost is super-linear: 2k={t2k}ms 20k={t20k}ms (ceiling {ceiling}ms)",
        )

    def test_4857_eval_ir_hygienic_macro_is_linear(self) -> None:
        # --ir goes through CompilerService::eval_ir, the face that used to
        # hand the whole macro-bearing flat to the walker.
        proc = _run([str(AURA_BIN), "--ir", _HYGIENIC], cwd=ROOT)
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        self._assert_linear("eval_ir", proc.stdout)
        self.assertIn("1999000", proc.stdout)
        self.assertIn("199990000", proc.stdout)

    def test_4857_file_face_hygienic_macro_is_linear(self) -> None:
        # The default oneshot file face goes through CompilerService::eval.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "macro_loop_4857.aura"
            path.write_text(_HYGIENIC, encoding="utf-8")
            proc = subprocess.run(
                [str(AURA_BIN), str(path)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                env=_env(),
                timeout=90,
            )
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        self._assert_linear("eval", proc.stdout)

    def test_4857_mixed_legacy_macro_does_not_slow_hygienic_calls(self) -> None:
        # A legacy MacroDef used to route the whole flat to the runtime
        # walker, re-expanding the hygienic call per iteration. Both faces
        # must static-expand the hygienic call first.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "mixed_macro_loop_4857.aura"
            path.write_text(_MIXED, encoding="utf-8")
            proc = subprocess.run(
                [str(AURA_BIN), str(path)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                env=_env(),
                timeout=90,
            )
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        self._assert_linear("mixed eval", proc.stdout)


if __name__ == "__main__":
    unittest.main()
