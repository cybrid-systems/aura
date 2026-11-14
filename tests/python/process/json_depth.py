#!/usr/bin/env python3
"""Regression for #5152.

json-parse recursed once per nesting level and SIGSEGV'd on a deep
document. Nesting deeper than 512 is a catchable `json-parse: nesting too
deep` error. The process stays up. Arrays, objects, a file payload, and an
iteratively built string all take that path. A 512-deep array still parses.
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

from _aura_harness import AURA_BIN  # noqa: E402


def _env() -> dict[str, str]:
    env = dict(os.environ)
    for k in ("AURA_PIPELINE_STRICT", "AURA_SANDBOX", "AURA_ARENA_MOVING_COMPACT", "AURA_PRESET"):
        env.pop(k, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    return env


def _lines(stdout: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for raw in stdout.splitlines():
        line = raw.strip()
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        out[key] = val
    return out


class JsonDepth5152(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not AURA_BIN.exists():
            raise unittest.SkipTest(f"aura binary not found: {AURA_BIN}")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        (cls.dir / "ok512.json").write_text("[" * 512 + "]" * 512)
        (cls.dir / "deep-arr.json").write_text("[" * 100_000 + "]" * 100_000)
        (cls.dir / "deep-obj.json").write_text('{"a":' * 100_000 + "1" + "}" * 100_000)
        (cls.dir / "depth.aura").write_text(
            r"""
(define one (json-parse "1"))
(display (format "one=~a\n" one))
(display (format "one-err=~a\n" (error? one)))
(define shallow (json-parse "[1,{\"a\":true}]"))
(display (format "shallow-err=~a\n" (error? shallow)))
(define ok512 (json-parse (read-file "ok512.json")))
(display (format "ok512=~a\n" (if (error? ok512) ok512 "ok")))
(define deep (try (json-parse (read-file "deep-arr.json")) (catch (e) e)))
(display (format "deep=~a\n" deep))
(define obj (try (json-parse (read-file "deep-obj.json")) (catch (e) e)))
(display (format "obj=~a\n" obj))
(define built
  (let loop ((i 0) (acc "0"))
    (if (>= i 600) acc (loop (+ i 1) (string-append "[" acc "]")))))
(define built-r (try (json-parse built) (catch (e) e)))
(display (format "built=~a\n" built-r))
(display "alive\n")
"""
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_depth_limit_is_catchable(self) -> None:
        p = subprocess.run(
            [str(AURA_BIN), "--preset", "dev", "depth.aura"],
            capture_output=True,
            text=True,
            env=_env(),
            cwd=self.dir,
            timeout=60,
            stdin=subprocess.DEVNULL,
        )
        blob = p.stdout + p.stderr
        self.assertEqual(p.returncode, 0, blob)
        self.assertNotIn("SIGSEGV", blob)
        self.assertIn("alive", p.stdout)
        got = _lines(p.stdout)
        self.assertEqual(got.get("one"), "1", got)
        self.assertEqual(got.get("one-err"), "#f", got)
        self.assertEqual(got.get("shallow-err"), "#f", got)
        self.assertEqual(got.get("ok512"), "ok", got)
        self.assertIn("nesting too deep", got.get("deep", ""))
        self.assertIn("nesting too deep", got.get("obj", ""))
        self.assertIn("nesting too deep", got.get("built", ""))

    def test_uncaught_repro_survives(self) -> None:
        prog = self.dir / "repro.aura"
        prog.write_text(
            "\n".join(
                [
                    "(define (build n ch)",
                    '  (let loop ((i 0) (acc ""))',
                    "    (if (>= i n) acc (loop (+ i 1) (string-append acc ch)))))",
                    '(define s (string-append (build 1000 "[") (build 1000 "]")))',
                    '(display (format "len=~a\\n" (string-length s)))',
                    '(display (format "parse => ~a\\n" (json-parse s)))',
                    '(display "alive\\n")',
                    "",
                ]
            )
        )
        p = subprocess.run(
            [str(AURA_BIN), "--preset", "dev", str(prog)],
            capture_output=True,
            text=True,
            env=_env(),
            cwd=self.dir,
            timeout=60,
            stdin=subprocess.DEVNULL,
        )
        blob = p.stdout + p.stderr
        self.assertNotIn("SIGSEGV", blob)
        self.assertNotIn("AURA CRASH", blob)
        self.assertIn("alive", p.stdout)
        self.assertIn("nesting too deep", p.stderr)
        self.assertEqual(p.returncode, 1, blob)

    def test_help_text_names_the_limit(self) -> None:
        src = (ROOT / "src/compiler/evaluator_primitives_json.cpp").read_text()
        self.assertIn("Nesting deeper than 512", src)
        self.assertIn("kJsonParseMaxNesting = 512", src)
        doc = (ROOT / "docs/stdlib/primitive-error-convention.md").read_text()
        self.assertIn("nesting deeper than 512", doc)
        lib = (ROOT / "lib/std/json.aura").read_text()
        self.assertIn("deeper than 512", lib)


if __name__ == "__main__":
    unittest.main()
