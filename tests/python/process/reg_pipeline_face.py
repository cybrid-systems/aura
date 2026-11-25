#!/usr/bin/env python3
"""Issue #5470: which face runs a file's top-level defines.

The 0.3.3 changelog claimed file/-e/load/require publish IR closures by
default. The tag actually ships the pre-#5420 whole-program face there:
the form-by-form IR face is the REPL and AURA_PIPELINE_STRICT=force-soa.
This pins both faces so the changelog cannot drift again.
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AURA_BIN = ROOT / "build" / "aura"

HOT = """
(define (loop n acc) (if (= n 0) acc (loop (- n 1) (+ acc 1))))
(define t0 (monotonic-ms))
(display (loop 2000000 0)) (newline)
(let ((cs (stats:get "closure:stats")))
  (display (string-append "took " (number->string (- (monotonic-ms) t0)) " ms"
                          " ir-calls=" (number->string (hash-ref cs "ir-calls"))
                          " tw-calls=" (number->string (hash-ref cs "tw-calls")))) (newline))
"""


class PipelineFace(unittest.TestCase):
    def run_face(self, workdir: Path, strict: str | None) -> tuple[int, int, int, str]:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        if strict is None:
            env.pop("AURA_PIPELINE_STRICT", None)
        else:
            env["AURA_PIPELINE_STRICT"] = strict
        proc = subprocess.run(
            [str(AURA_BIN), "hot.aura"],
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=120,
            env=env,
        )
        text = proc.stdout or ""
        self.assertEqual(proc.returncode, 0, text[-800:])
        self.assertIn("2000000", text, text[-800:])
        took = re.search(r"took (\d+) ms", text)
        ir = re.search(r"ir-calls=(\d+)", text)
        tw = re.search(r"tw-calls=(\d+)", text)
        self.assertIsNotNone(took, text[-800:])
        self.assertIsNotNone(ir, text[-800:])
        self.assertIsNotNone(tw, text[-800:])
        return int(took.group(1)), int(ir.group(1)), int(tw.group(1)), text

    def test_default_and_force_soa_faces(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        with tempfile.TemporaryDirectory(prefix="aura-pipeline-5470-") as td:
            workdir = Path(td)
            (workdir / "hot.aura").write_text(HOT)

            # The documented default: the whole-program face keeps the
            # tree-walker closures, even though the IR cache fills.
            took, ir, tw, text = self.run_face(workdir, None)
            self.assertLess(took, 10_000, text[-800:])
            self.assertEqual(ir, 0, text[-800:])
            self.assertGreaterEqual(tw, 2_000_000, text[-800:])

            # The documented escape hatch: form-by-form lowering hits IR.
            took, ir, tw, text = self.run_face(workdir, "force-soa")
            self.assertLess(took, 5_000, text[-800:])
            self.assertGreaterEqual(ir, 2_000_000, text[-800:])
            self.assertLessEqual(tw, 10, text[-800:])
