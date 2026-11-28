#!/usr/bin/env python3
"""Issue #5464: a warm require must keep the module's native tier.

The module's self-recursive function is compiled by the TW tier after a few
calls on a cold cache. On a warm (disk IR cache) require the same call used
to stay on the tree walker (2000ms vs 12ms). The hit path must restore the
per-name bookkeeping (function source) the tier re-lowering needs.
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

MODULE = """\
(define (modloop n acc) (if (= n 0) acc (modloop (- n 1) (+ acc 1))))
(export modloop)
"""

MAIN = """\
(require "./mod.aura")
(define t0 (monotonic-ms))
(display (modloop 2000000 0)) (newline)
(let ((cs (stats:get "closure:stats")))
  (display (string-append "took " (number->string (- (monotonic-ms) t0)) " ms"
                          " tw-calls=" (number->string (hash-ref cs "tw-calls"))))
  (newline))
"""


class ModuleCacheTier(unittest.TestCase):
    def run_once(self, workdir: Path, cache: Path) -> tuple[int, int, str]:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env["XDG_CACHE_HOME"] = str(cache)
        env["AURA_PERF_MAP"] = "1"
        env.pop("AURA_PIPELINE_STRICT", None)
        proc = subprocess.Popen(
            [str(AURA_BIN), "main.aura"],
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )
        stdout, _ = proc.communicate(timeout=60)
        text = stdout or ""
        took = re.search(r"took (\d+) ms", text)
        tw = re.search(r"tw-calls=(\d+)", text)
        self.assertEqual(proc.returncode, 0, text[-800:])
        self.assertIsNotNone(took, text[-800:])
        self.assertIsNotNone(tw, text[-800:])
        perf = Path("/tmp") / f"perf-{proc.pid}.map"
        perf_text = perf.read_text() if perf.exists() else ""
        perf.unlink(missing_ok=True)
        return int(took.group(1)), int(tw.group(1)), perf_text

    def run_program(self, workdir: Path, cache: Path, main: str = "main.aura") -> str:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env["XDG_CACHE_HOME"] = str(cache)
        env["AURA_MODULE_IR_BIND"] = "1"
        env.pop("AURA_PIPELINE_STRICT", None)
        proc = subprocess.run(
            [str(AURA_BIN), main],
            cwd=workdir,
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
        )
        text = (proc.stdout or "") + (proc.stderr or "")
        self.assertEqual(proc.returncode, 0, text[-1200:])
        return text

    def test_named_let_and_unsafe_set_with_module_env_binding(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        with tempfile.TemporaryDirectory(prefix="aura-modbind-5488-") as td:
            workdir = Path(td)
            cache = workdir / "cache"
            cache.mkdir()
            lib = workdir / "lib"
            lib.mkdir()

            (lib / "mod.aura").write_text(
                "(export modloop modval)\n"
                "(define modval 7)\n"
                "(define (modloop n acc)\n"
                "  (let loop ((i n) (a acc))\n"
                "    (if (= i 0) (+ a modval) (loop (- i 1) (+ a 1)))))\n"
            )
            (workdir / "main.aura").write_text(
                '(require "./lib/mod.aura")\n'
                "(display (modloop 10 0)) (newline)\n"
                '(let ((cs (stats:get "closure:stats")))\n'
                '  (display "ir-calls=") (display (hash-ref cs "ir-calls")) (newline))\n'
            )
            out = self.run_program(workdir, cache)
            self.assertIn("17", out)
            ir = re.search(r"ir-calls=(\d+)", out)
            self.assertIsNotNone(ir, out[-1200:])
            self.assertGreater(int(ir.group(1)), 0, out[-1200:])

            (lib / "recordq.aura").write_text(
                "(export record-hold record-hold! record-count)\n"
                "(define record-hold #f)\n"
                "(define record-count 0)\n"
                "(define (record-hold! v)\n"
                "  (set! record-hold v)\n"
                "  (set! record-count (+ record-count 1))\n"
                "  v)\n"
            )
            (workdir / "main.aura").write_text(
                '(require "./lib/recordq.aura")\n'
                "(record-hold! #t)\n"
                '(display "hold=") (display record-hold) (newline)\n'
                '(display "count=") (display record-count) (newline)\n'
            )
            out = self.run_program(workdir, cache)
            self.assertIn("hold=#t", out)
            self.assertIn("count=1", out)

    def test_cold_and_warm_reach_the_native_tier(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        with tempfile.TemporaryDirectory(prefix="aura-modcache-5464-") as td:
            workdir = Path(td)
            cache = workdir / "cache"
            cache.mkdir()
            (workdir / "mod.aura").write_text(MODULE)
            (workdir / "main.aura").write_text(MAIN)

            cold_ms, cold_tw, cold_map = self.run_once(workdir, cache)
            self.assertLess(cold_ms, 500, f"cold run took {cold_ms} ms")
            self.assertLess(cold_tw, 100_000, f"cold tw-calls={cold_tw}")
            self.assertIn("modloop", cold_map)

            warm_ms, warm_tw, warm_map = self.run_once(workdir, cache)
            self.assertLess(warm_ms, 500, f"warm run took {warm_ms} ms")
            self.assertLess(warm_tw, 100_000, f"warm tw-calls={warm_tw}")
            self.assertIn("modloop", warm_map)


if __name__ == "__main__":
    unittest.main()
