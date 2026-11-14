#!/usr/bin/env python3
"""Regression for #5169: stdlib calls names that exist.

query:callers-of, strategy:best-arm, and the aot getters are not
callables. inline-function counts query :calls, set-strategy falls
back to strategy:best, and the hot-update readers use stats:get.
check-success and diagnose are the exports those modules bind.

The refactor and strategy rows use --preset dev and leave AURA_SANDBOX
unset. check-success and heal require std/test, whose macros need
MacroSelfEvo, so those two rows set AURA_SANDBOX=off. The qualified-call
gate is the static half of the same fix.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))
sys.path.insert(0, str(ROOT / "scripts" / "coverage" / "checks"))

from _aura_harness import AURA_BIN  # noqa: E402
from check_stdlib_unbound_5169 import engine_names, unbound_qualified_calls  # noqa: E402

CHECK = ROOT / "scripts" / "coverage" / "checks" / "check_stdlib_unbound_5169.py"
DEV_CAPS = "aura: preset=dev sandbox=restricted caps=mutate,read,write (tenant=0, ws=0)"


def run_aura(program: str, *, preset_dev: bool = False, sandbox_off: bool = False) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
    if sandbox_off:
        env["AURA_SANDBOX"] = "off"
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    args = [str(AURA_BIN)]
    if preset_dev:
        args += ["--preset", "dev"]
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "prog.aura"
        path.write_text(program, encoding="utf-8")
        args += ["--load", str(path)]
        return subprocess.run(
            args,
            cwd=tmp,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )


def _src(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


class TestIssue5169StdlibUnbound(unittest.TestCase):
    def test_checker_accepts_tree(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(CHECK)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn("lib/std", proc.stdout)

    def test_checker_flags_a_missing_qualified_call(self) -> None:
        missing = unbound_qualified_calls('(query:callers-of "foo")\n')
        self.assertEqual(missing, [(1, "query:callers-of")])
        defined = "(define (strategy:best-arm st) st)\n(strategy:best-arm st)\n"
        self.assertEqual(unbound_qualified_calls(defined), [])
        inert = (
            "; (aot:get-module-version)\n"
            '"(aot:get-region-mask)"\n'
            "'(diagnose:diagnose x)\n"
            '(stats:get "aot:get-module-version")\n'
        )
        self.assertEqual(unbound_qualified_calls(inert, {"stats:get"}), [])
        # #\ " and #\; are character literals, not a string or a comment.
        char_lit = '(char=? c #\\")\n(define (qd:struct-hash x) x)\n(qd:struct-hash 1)\n'
        self.assertEqual(unbound_qualified_calls(char_lit), [])
        semi = "(char=? c #\\;) (define (foo:bar x) x) (foo:bar 1)\n"
        self.assertEqual(unbound_qualified_calls(semi), [])

    def test_engine_catalog_skips_stats_only_getters(self) -> None:
        names = engine_names()
        self.assertIn("aot:set-module-version", names)
        self.assertIn("aot:set-region-mask", names)
        self.assertIn("stats:get", names)
        self.assertIn("query:calls", names)
        self.assertIn("query:defines", names)
        self.assertIn("set-code", names)
        self.assertIn("eval-current", names)
        self.assertNotIn("aot:get-module-version", names)
        self.assertNotIn("aot:get-region-mask", names)
        self.assertNotIn("query:callers-of", names)
        self.assertNotIn("mutate:rebind", names)

    def test_refactor_inline_counts_callers(self) -> None:
        proc = run_aura(
            "\n".join(
                [
                    '(require "std/refactor" all:)',
                    '(set-code "(define (foo x) (+ x 1)) (define (bar y) (foo y))")',
                    '(display "REF5169 ")',
                    '(display (refactor:inline-function "foo"))',
                    "(newline)",
                    '(display "MISS5169 ")',
                    '(display (refactor:inline-function "missing"))',
                    "(newline)",
                    "",
                ]
            ),
            preset_dev=True,
        )
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(DEV_CAPS, proc.stderr)
        self.assertNotIn("unbound variable", blob)
        self.assertIn("REF5169 1", proc.stdout)
        self.assertIn("MISS5169 #f", proc.stdout)

    def test_strategy_non_string_uses_best(self) -> None:
        # hash-set copies through hash-set!, so the write needs the dev grant.
        proc = run_aura(
            "\n".join(
                [
                    '(require "std/evo" all:)',
                    '(display "EMPTY5169 ")',
                    "(display (strategy:active (strategy:set-strategy (strategy:new) (list 1 2))))",
                    "(newline)",
                    '(display "BEST5169 ")',
                    "(display (strategy:active",
                    '  (strategy:set-strategy (strategy:report-success (strategy:new) "fast") (list 9))))',
                    "(newline)",
                    "",
                ]
            ),
            preset_dev=True,
        )
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn(DEV_CAPS, proc.stderr)
        self.assertNotIn("unbound variable", blob)
        self.assertNotIn("strategy:best-arm", blob)
        self.assertIn("EMPTY5169 #f", proc.stdout)
        self.assertIn("BEST5169 fast", proc.stdout)

    def test_hot_update_reads_stats(self) -> None:
        proc = run_aura(
            "\n".join(
                [
                    '(require "std/hot-update" all:)',
                    "(hot-update:set-module-version! 4)",
                    '(display "VER5169 ")',
                    "(display (hot-update:get-module-version))",
                    "(newline)",
                    "(hot-update:set-region! 6)",
                    '(display "REG5169 ")',
                    "(display (hot-update:get-region))",
                    "(newline)",
                    "",
                ]
            )
        )
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertNotIn("unbound variable", blob)
        self.assertIn("VER5169 4", proc.stdout)
        self.assertIn("REG5169 6", proc.stdout)

    def test_region_get_reads_stats(self) -> None:
        proc = run_aura(
            "\n".join(
                [
                    '(require "std/hot-update-region" all:)',
                    "(aot:set-region-mask 3)",
                    '(display "RGN5169 ")',
                    "(display (region:get))",
                    "(newline)",
                    "",
                ]
            )
        )
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertNotIn("unbound variable", blob)
        self.assertIn("RGN5169 3", proc.stdout)

    def test_check_success_reports_a_match(self) -> None:
        proc = run_aura(
            "\n".join(
                [
                    '(require "std/test")',
                    '(display "YES5169 ")',
                    '(display (check-success "value 42" (list "42")))',
                    "(newline)",
                    '(display "NO5169 ")',
                    '(display (check-success "nope" (list "42")))',
                    "(newline)",
                    "",
                ]
            ),
            sandbox_off=True,
        )
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertNotIn("unbound variable", blob)
        self.assertIn("YES5169 #t", proc.stdout)
        self.assertIn("NO5169 #f", proc.stdout)

    def test_heal_diagnose_fallback_is_bound(self) -> None:
        proc = run_aura(
            "\n".join(
                [
                    '(require "std/heal" all:)',
                    '(display "HEAL5169 ")',
                    '(display (heal "(display 1)" (list "nope") 1))',
                    "(newline)",
                    "",
                ]
            ),
            sandbox_off=True,
        )
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertNotIn("unbound variable", blob)
        self.assertIn("HEAL5169 ", proc.stdout)
        self.assertTrue("failed" in proc.stdout or "healed" in proc.stdout, proc.stdout)

    def test_sources_call_real_names(self) -> None:
        refactor = _src("lib/std/refactor.aura")
        self.assertNotIn("(query:callers-of", refactor)
        self.assertIn("(query :defines", refactor)
        self.assertIn("(query :calls", refactor)

        evo = _src("lib/std/evo.aura")
        self.assertNotIn("(strategy:best-arm", evo)
        self.assertIn("(strategy:best st)", evo)

        bench = _src("lib/std/bench.aura")
        self.assertNotIn("test:check-success", bench)
        self.assertIn("(check-success actual-str expects)", bench)

        heal = _src("lib/std/heal.aura")
        self.assertNotIn("(diagnose:diagnose", heal)
        self.assertNotIn("(test:check-success", heal)
        self.assertIn("(diagnose output)", heal)
        self.assertIn("(check-success output expected)", heal)

        hot = _src("lib/std/hot-update.aura")
        self.assertNotIn("(aot:get-module-version", hot)
        self.assertNotIn("(aot:get-region-mask", hot)
        self.assertIn('(stats:get "aot:get-module-version")', hot)
        self.assertIn('(stats:get "aot:get-region-mask")', hot)

        region = _src("lib/std/hot-update-region.aura")
        self.assertNotIn("(aot:get-region-mask", region)
        self.assertIn('(stats:get "aot:get-region-mask")', region)

        proposer = _src("lib/std/proposer.aura")
        self.assertIn('(require "std/mutate" all:)', proposer)
