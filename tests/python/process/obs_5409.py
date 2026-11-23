"""Issue #5409: external observability acceptance checks.

These run the real aura binary and inspect the artifacts external tools use:
/tmp/perf-<pid>.map for JIT Lisp names, .note.stapsdt for USDT probes, and
the dynamic symbol table for the exported interpreter entry points.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(ROOT / "tests" / "python"))
from _aura_harness import AURA_BIN  # noqa: E402

AURA = AURA_BIN


def run_aura(program: str, env: dict[str, str] | None = None, timeout: int = 120):
    e = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PATH", "AURA_EVO_LOG"):
        e.pop(key, None)
    if env:
        e.update(env)
    return subprocess.run(
        [str(AURA), "--jit"],
        input=program,
        capture_output=True,
        text=True,
        timeout=timeout,
        env=e,
    )


class PerfMap(unittest.TestCase):
    def test_jit_names_are_in_the_perf_map(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            proc = subprocess.Popen(
                [str(AURA), "--jit"],
                cwd=td,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "AURA_PERF_MAP": "1", "XDG_CACHE_HOME": td},
            )
            perf_map = Path("/tmp") / f"perf-{proc.pid}.map"
            perf_map.unlink(missing_ok=True)
            out, err = proc.communicate(
                "(begin (define (hot x) (+ x x)) (hot 21))\n", timeout=120
            )
            self.assertEqual(proc.returncode, 0, err[-800:])
            self.assertIn("42", out, err[-800:])
            self.assertTrue(perf_map.is_file(), f"missing {perf_map}")
            text = perf_map.read_text()
            self.assertRegex(text, r"(?m)^[0-9a-f]+ [0-9a-f]+ Lisp:")
            self.assertIn("Lisp:hot", text)

    def test_perf_map_is_opt_in(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            proc = subprocess.Popen(
                [str(AURA), "--jit"],
                cwd=td,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={
                    **{k: v for k, v in os.environ.items() if k != "AURA_PERF_MAP"},
                    "XDG_CACHE_HOME": td,
                },
            )
            existing = Path("/tmp") / f"perf-{proc.pid}.map"
            existing.unlink(missing_ok=True)
            proc.communicate("(display (+ 1 2))\n", timeout=120)
            self.assertEqual(proc.returncode, 0)
            self.assertFalse(existing.exists())


class Usdt(unittest.TestCase):
    def test_probe_notes_are_in_the_binary(self) -> None:
        readelf = shutil.which("readelf")
        if not readelf:
            self.skipTest("readelf not installed")
        r = subprocess.run([readelf, "-n", str(AURA)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr[-400:])
        for needle in (
            "Provider: aura",
            "fiber__spawn",
            "fiber__yield",
            "fiber__resume",
            "channel__wait_enter",
            "channel__wait_exit",
            "gc__start",
            "gc__end",
            "http_post__start",
            "http_post__end",
            "require_eval__start",
            "require_eval__end",
        ):
            self.assertIn(needle, r.stdout)

    def test_runtime_symbols_are_exported(self) -> None:
        nm = shutil.which("nm")
        if not nm:
            self.skipTest("nm not installed")
        r = subprocess.run([nm, "-D", str(AURA)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr[-400:])
        self.assertIn("eval_flat_body", r.stdout)
