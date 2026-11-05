#!/usr/bin/env python3
"""Regression for #4910: evolution is observable from one command.

mutate:rebind / ast:restore / evo:record! append worldline records to
AURA_EVO_LOG (`aura run` in a project defaults it to .aura/evo/log.jsonl);
`aura evo log [--json]` lists them (id, parent, decision, reason, evidence,
diff, duration) and `aura evo report --html DIR` writes a static report.
Before: no such log or command existed (`aura evo` ran "evo" as a file).
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

PROG = (
    '(set-code "(define (score x) (* x 2))")\n'
    "(eval-current)\n"
    '(define s (ast:snapshot "base"))\n'
    '(display (list "rb" (mutate:rebind "score" "(lambda (x) (* x 3))" "triple"))) (newline)\n'
    "(eval-current)\n"
    '(display (list "id" (evo:record! "score-v2" "keep" "holdout 0.71 >= 0.70"'
    ' (hash "score" 0.71 "threshold" 0.7) "(define (score x) (* x 3))" 12))) (newline)\n'
    '(display (list "bad" (mutate:rebind "score" "(lambda (x)" "broken"))) (newline)\n'
    '(display (list "restore" (ast:restore s))) (newline)\n'
    '(display (list "rej" (evo:record! "score-v3" "rollback" "holdout 0.52 < 0.70"'
    ' (hash "score" 0.52 "threshold" 0.7)))) (newline)\n'
)


def env_for(log: Path | None) -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    env.pop("AURA_EVO_LOG", None)
    env.pop("AURA_EVO_RUN", None)
    if log is not None:
        env["AURA_EVO_LOG"] = str(log)
    return env


def aura(args: list[str], cwd: str, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120)


class EvoLog(unittest.TestCase):
    def test_log_records_and_cli(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(PROG)
            log = Path(d) / "evo" / "log.jsonl"
            r = aura(["p.aura"], d, env_for(log))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(log.is_file(), r.stdout + r.stderr)
            recs = [json.loads(line) for line in log.read_text().splitlines()]
            kinds = [(x["kind"], x["decision"]) for x in recs]
            self.assertIn(("mutate:rebind", "KEEP"), kinds)
            self.assertIn(("record", "KEEP"), kinds)
            self.assertIn(("mutate:rebind", "ROLLBACK"), kinds)
            self.assertIn(("ast:restore", "ROLLBACK"), kinds)
            self.assertIn(("record", "ROLLBACK"), kinds)
            keep = next(x for x in recs if x["kind"] == "mutate:rebind" and x["decision"] == "KEEP")
            self.assertEqual(keep["target"], "score")
            self.assertIn("* x 2", keep["old"])
            self.assertIn("* x 3", keep["new"])
            self.assertEqual(keep["parent"], "")
            rec = next(x for x in recs if x["kind"] == "record" and x["decision"] == "KEEP")
            self.assertEqual(rec["parent"], keep["id"])  # worldline tip
            self.assertEqual(rec["evidence"], {"score": 0.71, "threshold": 0.7})
            self.assertEqual(rec["reason"], "holdout 0.71 >= 0.70")
            self.assertEqual(rec["dur_ms"], 12)
            bad = next(x for x in recs if x["kind"] == "mutate:rebind" and x["decision"] == "ROLLBACK")
            self.assertTrue(bad["reason"], bad)
            self.assertIn(rec["id"], r.stdout)  # evo:record! returns the id

            text = aura(["evo", "log", "--log", str(log)], d, env_for(None))
            self.assertEqual(text.returncode, 0, text.stderr)
            self.assertIn("KEEP", text.stdout)
            self.assertIn("ROLLBACK", text.stdout)
            self.assertIn("reason:   holdout 0.52 < 0.70", text.stdout)
            self.assertIn("evidence: score=0.71 threshold=0.7", text.stdout)
            self.assertIn("diff:     +", text.stdout)
            # evo:record! verdicts are the candidates; automatic records are listed only.
            self.assertIn("(2 candidates: 1 KEEP, 1 ROLLBACK)", text.stdout)
            self.assertNotIn("mutate:rebind", text.stdout)
            full = aura(["evo", "log", "--all", "--log", str(log)], d, env_for(None))
            self.assertEqual(full.returncode, 0, full.stderr)
            self.assertIn("mutate:rebind", full.stdout)
            self.assertIn("ast:restore", full.stdout)

            js = aura(["evo", "log", "--json", "--run", "last", "--log", str(log)], d, env_for(None))
            self.assertEqual(js.returncode, 0, js.stderr)
            self.assertEqual(len(json.loads(js.stdout)), len(recs))

            out = Path(d) / "report"
            rep = aura(["evo", "report", "--html", str(out), "--log", str(log)], d, env_for(None))
            self.assertEqual(rep.returncode, 0, rep.stderr)
            page = (out / "index.html").read_text()
            self.assertIn("<svg", page)
            self.assertIn('class="ROLLBACK"', page)
            self.assertIn('class="add"', page)
            self.assertIn("holdout 0.71 &gt;= 0.70", page)
            self.assertNotIn("http://", page.replace("http://www.w3.org", ""))

    def test_off_by_default_outside_projects(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text('(display (evo:record! "c" "KEEP" "r" 1))\n')
            r = aura(["p.aura"], d, env_for(None))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "#f")
            self.assertFalse((Path(d) / ".aura").exists())
            miss = aura(["evo", "log"], d, env_for(None))
            self.assertEqual(miss.returncode, 1)
            self.assertIn("hint:", miss.stderr)

    def test_project_run_defaults_log(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "aura.toml").write_text('entry = "main.aura"\n')
            (Path(d) / "main.aura").write_text('(display (evo:record! "c" "KEEP" "first" 1))\n')
            r = aura(["run", "--quiet"], d, env_for(None))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue((Path(d) / ".aura" / "evo" / "log.jsonl").is_file())
            text = aura(["evo", "log"], d, env_for(None))
            self.assertEqual(text.returncode, 0, text.stderr)
            self.assertIn("first", text.stdout)


if __name__ == "__main__":
    unittest.main()
