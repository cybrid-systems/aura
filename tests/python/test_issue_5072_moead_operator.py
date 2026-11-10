#!/usr/bin/env python3
"""#5072: unknown moead operators are refused; "rand" is audited as random."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

PROG = r"""
(require "std/moead" all:)
(define s (moead:make (hash "n-obj" 2 "H" 4 "T" 4 "dim" 3 "seed" 1
                            "bounds" (list (list 0.0 1.0))
                            "operator" "rand")))
(display (hash-ref (moead:report s) "operator-meaning"))
(newline)
"""


def aura(args: list[str], cwd: str, log: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    env["AURA_EVO_LOG"] = str(log)
    return subprocess.run(
        [str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120
    )


class MoeadOperatorAudit(unittest.TestCase):
    def test_rand_is_recorded_as_random(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(PROG)
            log = Path(d) / "evo.jsonl"
            r = aura(["p.aura"], d, log)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("random", r.stdout)
            recs = [json.loads(line) for line in log.read_text().splitlines() if line.strip()]
            notes = [x for x in recs if x.get("decision") == "OPERATOR"]
            self.assertTrue(notes, recs)
            self.assertIn("uniform random", notes[0]["reason"])
            self.assertIn("not differential evolution", notes[0]["reason"])
            self.assertEqual(notes[0]["evidence"]["meaning"], "random")


if __name__ == "__main__":
    unittest.main()
