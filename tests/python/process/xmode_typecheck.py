#!/usr/bin/env python3
"""Cross-mode: the typecheck oracle, the mutate gate and --typecheck agree.

Issue #5250. One program, three faces:

  * in-process  (set-code) + (typecheck-status) under the production pack
                (aura --preset dev, where the gate is fail-closed),
  * the gate    (mutate :rebind) with the same ill-typed lambda body,
  * the CLI     (aura --typecheck main.aura).

Before the fix the oracle answered "clean" for (define (g y) (+ y "s")) while
the gate failed closed and --typecheck printed a type error: the oracle
trusted per-node types a whole-workspace install had stamped and dropped the
diagnostics of that traversal. The same file also pins #5250's supplement:
--typecheck follows (require ...), so a program whose names come from a lib/
module is not reported as unbound by the static face.

The production pack is what makes the gate reject, so this comparison cannot
live in tests/suite (Soft) — it is a cross-mode process check.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

GOOD = "(define (g y) (+ y 1))"
BAD = '(define (g y) (+ y "s"))'
BAD_LAMBDA = '(lambda (y) (+ y "s"))'
GOOD_LAMBDA = "(lambda (y) (+ y 2))"


def _env() -> dict[str, str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    return env


def run_aura(args: list[str], cwd: Path, text: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(AURA_BIN), *args],
        input=text,
        cwd=cwd,
        env=_env(),
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )


def oracle_face(work: Path, code: str) -> str:
    """In-process conclusion under the production pack: clean|error."""
    prog = work / "oracle.aura"
    prog.write_text(
        f'(set-code {json.dumps(code)})\n(eval-current)\n(display (if (eq? (typecheck-status) #t) "clean" "error"))\n',
        encoding="utf-8",
    )
    done = run_aura(["--preset", "dev", "oracle.aura"], work)
    return done.stdout.strip()


def gate_face(work: Path, lambda_body: str) -> str:
    """The post-mutate gate's verdict on lambda_body: accepted|rejected."""
    prog = work / "gate.aura"
    prog.write_text(
        '(require "std/mutate" all:)\n'
        f"(set-code {json.dumps(GOOD)})\n(eval-current)\n"
        f'(define r (try (mutate :rebind "g" {json.dumps(lambda_body)} "xmode") (catch (e) (list "raised" e))))\n'
        '(display (if (eq? r #t) "accepted" "rejected"))\n',
        encoding="utf-8",
    )
    done = run_aura(["--preset", "dev", "gate.aura"], work)
    return done.stdout.strip()


def cli_face(work: Path, source: str, name: str = "main.aura") -> subprocess.CompletedProcess[str]:
    (work / name).write_text(source, encoding="utf-8")
    return run_aura(["--typecheck", name], work)


class TypecheckThreePaths(unittest.TestCase):
    def test_ill_typed_body_every_face(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            self.assertEqual(oracle_face(work, BAD), "error")
            self.assertEqual(gate_face(work, BAD_LAMBDA), "rejected")
            cli = cli_face(work, BAD + "\n")
            blob = cli.stdout + cli.stderr
            self.assertEqual(cli.returncode, 1, blob)
            self.assertIn("type error", cli.stdout)
            self.assertNotIn("no errors", cli.stdout)

    def test_legal_body_every_face(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            self.assertEqual(oracle_face(work, GOOD), "clean")
            self.assertEqual(gate_face(work, GOOD_LAMBDA), "accepted")
            cli = cli_face(work, GOOD + "\n")
            blob = cli.stdout + cli.stderr
            self.assertEqual(cli.returncode, 0, blob)
            self.assertIn("no errors", cli.stdout)
            self.assertNotIn("unbound variable", blob)

    def test_typecheck_follows_require(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / "lib").mkdir()
            (work / "lib" / "m.aura").write_text("(define (pi-ish) 3.14)\n", encoding="utf-8")
            entry = '(require "lib/m" all:)\n(display (pi-ish))\n(display pi-ish)\n'
            cli = cli_face(work, entry)
            blob = cli.stdout + cli.stderr
            self.assertEqual(cli.returncode, 0, blob)
            self.assertIn("no errors", cli.stdout)
            self.assertNotIn("unbound variable", blob)
            # The runtime face agrees with the static one.
            ran = run_aura(["main.aura"], work)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("3.14", ran.stdout)

    def test_required_file_diagnostic_names_that_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / "lib").mkdir()
            (work / "lib" / "bad.aura").write_text('(define (oops y) (+ y "s"))\n', encoding="utf-8")
            cli = cli_face(work, '(require "lib/bad" all:)\n(display 1)\n')
            blob = cli.stdout + cli.stderr
            self.assertEqual(cli.returncode, 1, blob)
            self.assertIn("bad.aura:", cli.stdout)
            self.assertIn("type error", cli.stdout)
