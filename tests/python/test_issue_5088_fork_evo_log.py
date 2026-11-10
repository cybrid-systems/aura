#!/usr/bin/env python3
"""Regression for #5088: a child-workspace mutate is not a live KEEP.

evo:trial rewrites the candidate in a forked workspace. The engine used to
append decision=KEEP for that mutate:rebind, which reads as a live commit
even though the process (or the child workspace) discards it. The line is
now decision=FORK with scope=fork, and it does not become the worldline tip.
"""

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

PROG = """
(set-code "(define (score x) (* x 2))")
(eval-current)
(mutate:rebind "score" "(lambda (x) (* x 3))" "live")
(define child (workspace :create "trial"))
(workspace :switch child)
(mutate:rebind "score" "(lambda (x) (* x 9))" "fork")
(mutate:rebind "score" "(lambda (x)" "broken-fork")
(workspace :switch 0)
(mutate:rebind "score" "(lambda (x) (* x 4))" "live-again")
(display "done")
(newline)
"""


def env_for(log: Path) -> dict[str, str]:
    out = dict(os.environ)
    out["AURA_SANDBOX"] = "off"
    out["AURA_EVO_LOG"] = str(log)
    out.pop("AURA_EVO_RUN", None)
    out.pop("AURA_PIPELINE_STRICT", None)
    return out


class ForkEvoLog(unittest.TestCase):
    def test_child_rebind_is_fork_not_keep(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(PROG)
            log = Path(d) / "evo.jsonl"
            r = subprocess.run(
                [str(AURA_BIN), "--quiet", "p.aura"],
                cwd=d,
                capture_output=True,
                text=True,
                env=env_for(log),
                timeout=120,
            )
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            self.assertTrue(log.is_file(), r.stdout + r.stderr)
            recs = [json.loads(line) for line in log.read_text().splitlines() if line.strip()]
            rebinds = [x for x in recs if x["kind"] == "mutate:rebind"]
            self.assertGreaterEqual(len(rebinds), 4, recs)
            live = [x for x in rebinds if x["decision"] == "KEEP"]
            forks = [x for x in rebinds if x["decision"] == "FORK"]
            self.assertEqual(len(live), 2, rebinds)
            self.assertEqual(len(forks), 1, rebinds)
            self.assertEqual(forks[0]["scope"], "fork")
            self.assertEqual(forks[0]["reason"], "fork-worldline")
            self.assertIn("* x 9", forks[0]["new"])
            self.assertNotIn("scope", live[0])
            broken = next(x for x in rebinds if x["decision"] == "ROLLBACK")
            self.assertEqual(broken["scope"], "fork")
            # The later live KEEP parents the previous live KEEP, not the fork.
            self.assertEqual(live[1]["parent"], live[0]["id"])
            self.assertNotEqual(live[1]["parent"], forks[0]["id"])


if __name__ == "__main__":
    unittest.main()
