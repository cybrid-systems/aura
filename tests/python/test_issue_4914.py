#!/usr/bin/env python3
"""Regression for #4914: `mutate:rebind` under `aura soft` matches the unchecked run.

Before: in `aura soft` (production defaults) `(mutate :rebind "params" "(list 1 2)")`
and `(quote (1 2 3))` were rejected with a generic "persist-reject ... topology
restored" (the real reason was dropped), quoted alists failed as "unbound
variable: fast", `ast:restore` after a rebind failed the linear-boundary gate,
and one rejected rebind left the panic checkpoint live, so densify-throttle
refused every later admit. Now soft output equals AURA_SANDBOX=off for valid
rebinds, a rejected rebind names its reason, and the workspace recovers.
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

REBIND = '(set-code "(define params (quote ((fast . 2) (slow . 5))))")\n(eval-current)\n(display (list "v1" params)) (newline)\n(set-code "(define params (quote ((fast . 9) (slow . 5))))")\n(eval-current)\n(display (list "v2 set-code" params)) (newline)\n(define s (mutate :snapshot "a"))\n(display (list "rb-list" (mutate :rebind "params" "(list 1 2)" "x"))) (newline)\n(eval-current)\n(display (list "v3" params)) (newline)\n(display (list "rb-quote" (mutate :rebind "params" "(quote (1 2 3))" "x"))) (newline)\n(eval-current)\n(display (list "v4" params)) (newline)\n(display (list "restore" (mutate :restore s))) (newline)\n(eval-current)\n(display (list "v5" params)) (newline)\n'

REBIND_DATA = '(set-code "(define params (quote ((fast . 2) (slow . 5))))")\n(eval-current)\n(define s (mutate :snapshot "a"))\n(display (list "rb-alist" (mutate :rebind "params" "(quote ((fast . 3) (slow . 7)))" "x"))) (newline)\n(eval-current)\n(display (list "v" params)) (newline)\n(set-code "(define (get-params) (quote ((fast . 2) (slow . 5))))")\n(eval-current)\n(display (list "rb-lam" (mutate :rebind "get-params" "(lambda () (quote ((fast . 4) (slow . 8) (vol . #t))))" "x"))) (newline)\n(eval-current)\n(display (list "g" (get-params))) (newline)\n(display (list "rb-strs" (mutate :rebind "get-params" "(lambda () (quote (\\"a\\" 1.5 sym)))" "x"))) (newline)\n(eval-current)\n(display (list "g2" (get-params))) (newline)\n'

REJECT_THEN_RECOVER = '(set-code "(define params (quote ((fast . 2) (slow . 5))))")\n(eval-current)\n(define s (mutate :snapshot "a"))\n(display (list "bad" (mutate :rebind "params" "(+ 1 \\"x\\")" "x"))) (newline)\n(eval-current)\n(display (list "v1" params)) (newline)\n(display (list "good" (mutate :rebind "params" "(quote (7 8))" "x"))) (newline)\n(eval-current)\n(display (list "v2" params)) (newline)\n(display (list "restore" (mutate :restore s))) (newline)\n(eval-current)\n(display (list "v3" params)) (newline)\n'


def _run(src: str, soft: bool) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(k, None)
    if not soft:
        env["AURA_SANDBOX"] = "off"
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        args = [str(AURA_BIN), "soft", path] if soft else [str(AURA_BIN), path]
        return subprocess.run(args, capture_output=True, text=True, env=env, timeout=120)
    finally:
        os.unlink(path)


def _lines(out: str) -> list[str]:
    return [ln for ln in out.splitlines() if ln.startswith("(")]


class SoftRebindMatchesOff(unittest.TestCase):
    def test_list_quote_restore(self) -> None:
        soft, off = _run(REBIND, True), _run(REBIND, False)
        self.assertEqual(_lines(soft.stdout), _lines(off.stdout), soft.stdout + soft.stderr)
        self.assertIn("(rb-list #t)", soft.stdout)
        self.assertIn("(v4 (1 2 3))", soft.stdout)
        self.assertIn("(restore #t)", soft.stdout)
        self.assertIn("(v5 ((fast . 9) (slow . 5)))", soft.stdout)

    def test_alist_and_lambda_data(self) -> None:
        soft, off = _run(REBIND_DATA, True), _run(REBIND_DATA, False)
        self.assertEqual(_lines(soft.stdout), _lines(off.stdout), soft.stdout + soft.stderr)
        self.assertIn("(v ((fast . 3) (slow . 7)))", soft.stdout)

    def test_reject_names_reason_and_recovers(self) -> None:
        r = _run(REJECT_THEN_RECOVER, True)
        out = r.stdout
        self.assertNotIn("persist-reject", out, out + r.stderr)
        self.assertIn("typecheck", out)
        self.assertIn("(v1 ((fast . 2) (slow . 5)))", out)
        self.assertIn("(good #t)", out, out + r.stderr)
        self.assertIn("(v2 (7 8))", out)
        self.assertIn("(restore #t)", out)
        self.assertIn("(v3 ((fast . 2) (slow . 5)))", out)


if __name__ == "__main__":
    unittest.main()
