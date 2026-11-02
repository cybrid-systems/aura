#!/usr/bin/env python3
"""Issue #4864: the oneshot file face must agree with the --load face.

#4848 made "aura --load file.aura" take the permissive walker face (the
production pack default is Forbidden, and the Forbidden gate vetoes
ordinary forms the IR path cannot express: let-bound closures, named
let, set!, do, try, internal defines, hygienic macros).

The ONESHOT file entry ("aura file.aura", no --load) never took that
face, so the same program was rejected with

    internal error: tree-walker fallback forbidden in production pipeline

while "aura --load file.aura" ran it. This test runs the same file
through both faces and compares the result.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

# A file with the forms the Forbidden gate rejects.
PROGRAMS = {
    "let": "(display (let ((a 1) (b 2)) (+ a b)))\n",
    "named-let": "(define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc n))))\n(display (loop 10 0))\n",
    "set!": "(define x 1)\n(set! x 2)\n(display x)\n",
    "do": "(display (do ((i 0 (+ i 1)) (acc 0 (+ acc i))) ((= i 5) acc)))\n",
    "try": '(display (try (error "boom") (catch (e) "caught")))\n',
    "internal-define": "(define (f) (define y 4) (* y y))\n(display (f))\n",
    "hygienic-macro": "(define-hygienic-macro (or2-h a b) (let ((t a)) (if t t b)))\n(display (or2-h #f 3))\n",
    "lambda-arg": '(define (probe th) (try (th) (catch (e) (list (quote caught) (format "~a" e)))))\n'
    "(probe (lambda () (+ 1 2)))\n",
}


def _env() -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    return env


class TestIssue4864(unittest.TestCase):
    def test_oneshot_file_face_matches_load_face(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        for name, source in PROGRAMS.items():
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / f"{name}.aura"
                path.write_text(source, encoding="utf-8")
                env = _env()
                # The production face: no AURA_PIPELINE_STRICT override.
                env.pop("AURA_PIPELINE_STRICT", None)
                oneshot = subprocess.run(
                    [str(AURA_BIN), str(path)],
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=60,
                )
                loaded = subprocess.run(
                    [str(AURA_BIN), "--load", str(path)],
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=60,
                )
                self.assertNotIn(
                    "tree-walker fallback forbidden",
                    oneshot.stderr,
                    f"{name}: the oneshot file face must not veto the walker",
                )
                self.assertEqual(
                    loaded.stdout,
                    oneshot.stdout,
                    f"{name}: oneshot stdout {oneshot.stdout!r} != load stdout {loaded.stdout!r}",
                )
                self.assertEqual(
                    loaded.returncode,
                    oneshot.returncode,
                    f"{name}: oneshot rc {oneshot.returncode} != load rc {loaded.returncode}",
                )


if __name__ == "__main__":
    unittest.main()
