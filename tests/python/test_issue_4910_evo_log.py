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
    '(define s (mutate :snapshot "base"))\n'
    '(display (list "rb" (mutate :rebind "score" "(lambda (x) (* x 3))" "triple"))) (newline)\n'
    "(eval-current)\n"
    '(display (list "id" (evo:record! "score-v2" "keep" "holdout 0.71 >= 0.70"'
    ' (hash "score" 0.71 "threshold" 0.7) "(define (score x) (* x 3))" 12))) (newline)\n'
    '(display (list "bad" (mutate :rebind "score" "(lambda (x)" "broken"))) (newline)\n'
    '(display (list "restore" (mutate :restore s))) (newline)\n'
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


TIP_PROG = (
    '(set-code "(define (score x) (* x 2))")\n'
    "(eval-current)\n"
    '(display (list "c1" (evo:record! "c1" "keep" "first candidate"'
    ' (hash "score" 0.7) "(define (score x) (* x 2))"))) (newline)\n'
    '(display (list "rb" (mutate :rebind "score" "(lambda (x) (* x 3))" "triple"))) (newline)\n'
    '(display (list "c2" (evo:record! "c2" "keep" "second candidate"'
    ' (hash "score" 0.8) "(define (score x) (* x 3))"))) (newline)\n'
)


class EvoLog(unittest.TestCase):
    def test_record_tip_skips_auto_logged_rebinds(self) -> None:
        # Issue #5139: an auto-logged mutate:rebind carries decision=KEEP but
        # must not become the parent of the next record, or every candidate in
        # the report loses its lineage edge.
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(TIP_PROG)
            log = Path(d) / "evo" / "log.jsonl"
            r = aura(["p.aura"], d, env_for(log))
            self.assertEqual(r.returncode, 0, r.stderr)
            recs = [json.loads(line) for line in log.read_text().splitlines()]
            c1 = next(x for x in recs if x["reason"] == "first candidate")
            rb = next(x for x in recs if x["kind"] == "mutate:rebind")
            c2 = next(x for x in recs if x["reason"] == "second candidate")
            self.assertEqual(c1["parent"], "")
            self.assertEqual(rb["parent"], c1["id"])  # the rebind follows the record
            self.assertEqual(c2["parent"], c1["id"])  # record to record, not rebind
            rep = aura(
                ["evo", "report", "--html", str(Path(d) / "report"), "--run", "last", "--log", str(log)],
                d,
                env_for(None),
            )
            self.assertEqual(rep.returncode, 0, rep.stderr)
            self.assertNotIn("did not resolve", rep.stderr)

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
            self.assertIn(("mw:restore", "ROLLBACK"), kinds)
            self.assertIn(("record", "ROLLBACK"), kinds)
            keep = next(x for x in recs if x["kind"] == "mutate:rebind" and x["decision"] == "KEEP")
            self.assertEqual(keep["target"], "score")
            self.assertIn("* x 2", keep["old"])
            self.assertIn("* x 3", keep["new"])
            self.assertEqual(keep["parent"], "")
            rec = next(x for x in recs if x["kind"] == "record" and x["decision"] == "KEEP")
            # Issue #5139: a rebind KEEP is not a worldline tip, so the first
            # record starts the chain instead of inheriting the rebind id.
            self.assertEqual(rec["parent"], "")
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
            self.assertIn("(2 records: 1 KEEP, 1 ROLLBACK)", text.stdout)
            self.assertNotIn("mutate:rebind", text.stdout)
            full = aura(["evo", "log", "--all", "--log", str(log)], d, env_for(None))
            self.assertEqual(full.returncode, 0, full.stderr)
            self.assertIn("mutate:rebind", full.stdout)
            self.assertIn("mw:restore", full.stdout)

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

    def test_restore_appends_rollback_under_preset_dev(self) -> None:
        # Issue #5150: preset dev (restricted) must still append the
        # mw:restore ROLLBACK row, and `aura evo log` counts it.
        prog = (
            '(set-code "(define (f x) (* x 2))")\n'
            "(eval-current)\n"
            '(define snap (mutate :snapshot "s"))\n'
            '(mutate :rebind "f" "(define (f x) (+ x 100))" "candidate")\n'
            "(mutate :restore snap)\n"
            '(display "rolled back\\n")\n'
        )
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(prog)
            log = Path(d) / "evo.jsonl"
            env = dict(os.environ)
            env["AURA_EVO_LOG"] = str(log)
            env["AURA_RUNTIME_DIR"] = str(ROOT)
            env.pop("AURA_SANDBOX", None)
            env.pop("AURA_EVO_RUN", None)
            r = aura(["--preset", "dev", "p.aura"], d, env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            recs = [json.loads(line) for line in log.read_text().splitlines()]
            kinds = [(x["kind"], x["decision"]) for x in recs]
            self.assertIn(("mutate:rebind", "KEEP"), kinds)
            self.assertIn(("mw:restore", "ROLLBACK"), kinds)
            shown = aura(["evo", "log", "--log", str(log)], d, env_for(None))
            self.assertEqual(shown.returncode, 0, shown.stderr)
            self.assertIn("1 KEEP, 1 ROLLBACK", shown.stdout)
            self.assertIn("mw:restore", shown.stdout)

        # Issue #5139: the rollback row is not the worldline tip. The
        # record has to be in the same process — the tip is in-memory.
        tipped = prog + '(display (evo:record! "after" "keep" "after rollback"))\n(newline)\n'
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(tipped)
            log = Path(d) / "evo.jsonl"
            env = dict(os.environ)
            env["AURA_EVO_LOG"] = str(log)
            env["AURA_RUNTIME_DIR"] = str(ROOT)
            env.pop("AURA_SANDBOX", None)
            env.pop("AURA_EVO_RUN", None)
            r = aura(["--preset", "dev", "p.aura"], d, env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            recs = [json.loads(line) for line in log.read_text().splitlines()]
            restore = next(x for x in recs if x["kind"] == "mw:restore")
            after = next(x for x in recs if x["kind"] == "record")
            self.assertEqual(after["parent"], "")
            self.assertNotEqual(after["parent"], restore["id"])

    # ── Issue #4939: schema v=2 (evo-core) ──────────────────────────
    V2_PROG = (
        '(require "std/evo-core" all:)\n'
        "(define (eval-cand c)\n"
        '  (hash "objectives" (hash "a" 1.5 "b" 2.5)\n'
        '        "constraints" (hash "ok" 0.0)\n'
        '        "behavior" (hash "n" 3)))\n'
        '(define s (evo:search (hash "algo" "mock" "seed" 5 "budget" 2\n'
        '                      "pop" 2 "apply" (lambda (s c) #t))))\n'
        "(evo:run! s eval-cand)\n"
        '(define best (car (hash-ref (evo:report s) "archive")))\n'
        "(evo:commit! s best)\n"
        '(define kid (evo:child best "kid" (hash "target" "score")))\n'
        '(evo:tell! s (list kid) (list (hash "objectives" (hash "a" 0.5)\n'
        '                                    "constraints" (hash "ok" 0.0))))\n'
        "(evo:commit! s kid)\n"
        '(display (evo:record! "v1-line" "KEEP" "legacy v1" (hash "x" 1))) (newline)\n'
    )

    @staticmethod
    def _normalized(rec: dict) -> dict:
        """Drop the fields that name this process run (id/parent embed the
        run id; ts_ms/dur_ms are wall clock)."""
        return {k: v for k, v in rec.items() if k not in ("id", "parent", "run", "ts_ms", "dur_ms")}

    def test_v2_records_and_deterministic_reruns(self) -> None:
        runs: list[list[dict]] = []
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(self.V2_PROG)
            for i in range(3):
                log = Path(d) / f"evo{i}" / "log.jsonl"
                r = aura(["p.aura"], d, env_for(log))
                self.assertEqual(r.returncode, 0, r.stderr)
                recs = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertTrue(recs, "evo-core wrote nothing")
                runs.append(recs)
            # v=2 members are written by ask/trial/tell/commit records.
            v2 = [x for x in runs[0] if x["kind"] == "record" and x["v"] == 2]
            self.assertTrue(v2, [x.get("v") for x in runs[0]])
            scored = [x for x in v2 if x.get("objectives")]
            self.assertTrue(scored, "no v2 record carries objectives")
            rec = scored[0]
            for key in (
                "algo",
                "gen",
                "slot",
                "search",
                "candidate",
                "parents",
                "behavior",
                "objectives",
                "constraints",
            ):
                self.assertIn(key, rec)
            self.assertEqual(rec["objectives"], {"a": 1.5, "b": 2.5})
            self.assertEqual(rec["constraints"], {"ok": 0.0})
            # Aura's empty list IS the unit value, so a root candidate's
            # lineage encodes as null; a derived candidate carries ids.
            self.assertIn(rec["parents"], (None, []))
            self.assertEqual(rec["algo"], "mock")
            derived = [x for x in v2 if x["parents"] and x["candidate"] != x["parents"][0]]
            self.assertTrue(derived, "no v2 record carries multi-parent lineage")
            self.assertEqual(len(derived[0]["parents"]), 1)
            self.assertRegex(derived[0]["parents"][0], "^c[0-9a-f]+$")
            keeps = [x for x in v2 if x["decision"] == "KEEP"]
            self.assertTrue(keeps, "commit did not record a KEEP")
            # v=1 records still share the file, unchanged.
            v1 = [x for x in runs[0] if x["v"] == 1]
            self.assertTrue(v1)
            legacy = next(x for x in v1 if x["kind"] == "record")
            self.assertEqual(legacy["reason"], "legacy v1")
            self.assertNotIn("algo", legacy)
            # Three reruns, same seed: identical bytes modulo run id + clock.
            for other in runs[1:]:
                self.assertEqual(
                    [self._normalized(x) for x in runs[0]],
                    [self._normalized(x) for x in other],
                    "evo log is not reproducible across reruns",
                )
            # --json reads a v=1 + v=2 mixed file.
            log0 = Path(d) / "evo0" / "log.jsonl"
            js = aura(["evo", "log", "--json", "--log", str(log0)], d, env_for(None))
            self.assertEqual(js.returncode, 0, js.stderr)
            parsed = json.loads(js.stdout)
            self.assertEqual(len(parsed), len(runs[0]))
            self.assertEqual({x["v"] for x in parsed}, {1, 2})

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


class EvoReportQD(unittest.TestCase):
    """#4941: the report renders a QD heat map and the parent lineage from
    the behaviour/lineage members a v=2 log already carries."""

    LOG = [
        {
            "v": 2,
            "id": "r1#1",
            "parent": "",
            "run": "r1",
            "seq": 1,
            "ts_ms": 0,
            "kind": "record",
            "target": "c0",
            "decision": "KEEP",
            "reason": "seed",
            "evidence": {"score": 1.0},
            "old": "",
            "new": "",
            "dur_ms": 1.0,
            "algo": "mock",
            "gen": 0,
            "slot": 0,
            "search": "s",
            "candidate": "c0",
            "parents": [],
            "behavior": {"x": 0.0, "y": 0.0},
            "objectives": {"f": 1.0},
            "constraints": {},
        },
        {
            "v": 2,
            "id": "r1#2",
            "parent": "r1#1",
            "run": "r1",
            "seq": 2,
            "ts_ms": 0,
            "kind": "record",
            "target": "c1",
            "decision": "KEEP",
            "reason": "better",
            "evidence": {"score": 3.0},
            "old": "",
            "new": "",
            "dur_ms": 1.0,
            "algo": "mock",
            "gen": 1,
            "slot": 1,
            "search": "s",
            "candidate": "c1",
            "parents": ["c0", "c0b"],
            "behavior": {"x": 1.0, "y": 2.0},
            "objectives": {"f": 3.0},
            "constraints": {},
        },
        {
            "v": 2,
            "id": "r1#3",
            "parent": "r1#2",
            "run": "r1",
            "seq": 3,
            "ts_ms": 0,
            "kind": "record",
            "target": "c2",
            "decision": "ROLLBACK",
            "reason": "worse",
            "evidence": {"score": 2.0},
            "old": "",
            "new": "",
            "dur_ms": 1.0,
            "algo": "mock",
            "gen": 2,
            "slot": 2,
            "search": "s",
            "candidate": "c2",
            "parents": ["r1#2", "r1#1"],
            "behavior": {"x": 2.0, "y": 1.0},
            "objectives": {"f": 2.0},
            "constraints": {},
        },
    ]

    def test_qd_heatmap_and_lineage_svg(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "log.jsonl"
            log.write_text("".join(json.dumps(x) + "\n" for x in self.LOG))
            out = Path(d) / "report"
            rep = aura(["evo", "report", "--html", str(out), "--log", str(log)], d, env_for(None))
            self.assertEqual(rep.returncode, 0, rep.stderr)
            heat = out / "qd-heatmap.svg"
            lin = out / "lineage.svg"
            self.assertTrue(heat.is_file(), rep.stdout)
            self.assertTrue(lin.is_file(), rep.stdout)
            h = heat.read_text()
            self.assertIn('aria-label="QD heat map"', h)
            self.assertIn("<rect", h)
            page = (out / "index.html").read_text()
            self.assertIn("QD map", page)
            self.assertIn("Lineage", page)
            self.assertIn("<svg", page)
            tree = lin.read_text()
            self.assertIn('aria-label="lineage tree"', tree)
            self.assertIn("<circle", tree)
            self.assertIn("<line", tree)  # multi-parent edges are drawn

    def test_no_qd_records_means_no_empty_svg(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "log.jsonl"
            log.write_text(
                json.dumps(
                    {
                        "v": 1,
                        "id": "r#1",
                        "parent": "",
                        "run": "r",
                        "seq": 1,
                        "ts_ms": 0,
                        "kind": "record",
                        "target": "c",
                        "decision": "KEEP",
                        "reason": "plain",
                        "evidence": 1,
                        "old": "",
                        "new": "",
                        "dur_ms": 1.0,
                    }
                )
                + "\n"
            )
            out = Path(d) / "report"
            rep = aura(["evo", "report", "--html", str(out), "--log", str(log)], d, env_for(None))
            self.assertEqual(rep.returncode, 0, rep.stderr)
            self.assertFalse((out / "qd-heatmap.svg").exists())
            # A single record is still a lineage (one node); only an empty
            # behaviour space must not invent a heat map.
            if (out / "lineage.svg").exists():
                tree = (out / "lineage.svg").read_text()
                self.assertGreaterEqual(tree.count("<circle"), 1)


class EvoReportMultiObjective(unittest.TestCase):
    """#4948: Pareto scatter, HV curve, generation folds, surrogate audit and
    the `evo log --front/--lineage` selectors."""

    @staticmethod
    def _rec(i: int, objs: tuple[float, float], gen: int, parents: list[str], extra: dict | None = None) -> dict:
        rec = {
            "v": 2,
            "id": f"r#{i}",
            "parent": parents[0] if parents else "",
            "run": "r",
            "seq": i,
            "ts_ms": 0,
            "kind": "record",
            "target": f"c{i}",
            "decision": "KEEP" if i % 2 else "ROLLBACK",
            "reason": "gate",
            "evidence": {"score": objs[0]},
            "old": "",
            "new": "",
            "dur_ms": 1.0,
            "algo": "mock",
            "gen": gen,
            "slot": i,
            "search": "s",
            "candidate": f"c{i}",
            "parents": parents,
            "behavior": {"x": float(i), "y": float(4 - i)},
            "objectives": {"loss": objs[0], "cost": objs[1]},
            "constraints": {},
        }
        if extra:
            rec.update(extra)
        return rec

    def _write(self, d: str) -> Path:
        log = Path(d) / "log.jsonl"
        recs = [
            self._rec(1, (1.0, 4.0), 0, [], {"surrogate": {"mean": 0.2, "var": 0.1, "sent": True}}),
            self._rec(2, (2.0, 2.0), 0, ["r#1"]),
            self._rec(3, (3.0, 1.0), 1, ["r#2", "r#1"]),
            self._rec(4, (4.0, 3.0), 1, ["r#3"]),
        ]
        log.write_text("".join(json.dumps(x) + "\n" for x in recs))
        return log

    def test_report_pareto_hv_folds_and_surrogate(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = self._write(d)
            out = Path(d) / "report"
            rep = aura(["evo", "report", "--html", str(out), "--log", str(log)], d, env_for(None))
            self.assertEqual(rep.returncode, 0, rep.stderr)
            pareto = out / "pareto.svg"
            hv = out / "hv-curve.svg"
            self.assertTrue(pareto.is_file(), rep.stdout)
            self.assertTrue(hv.is_file(), rep.stdout)
            self.assertIn('aria-label="pareto scatter"', pareto.read_text())
            self.assertIn('aria-label="hypervolume curve"', hv.read_text())
            page = (out / "index.html").read_text()
            self.assertIn("Pareto front", page)
            self.assertIn("Hypervolume", page)
            self.assertIn("Surrogate pre-screen", page)
            self.assertIn("mean", page)
            # #4940: the representative (KEEP) trajectory on the score curve:
            # a best-so-far path through the KEEP records plus their markers.
            self.assertIn('aria-label="score curve"', page)
            self.assertIn('class="tip"', page)
            # Outer (generation) and inner (candidate) folds.
            self.assertEqual(page.count('class="fold"'), 2)
            self.assertIn("gen 0", page)
            self.assertIn("gen 1", page)

    def test_log_front_and_lineage(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = self._write(d)
            whole = aura(["evo", "log", "--log", str(log)], d, env_for(None))
            self.assertEqual(whole.returncode, 0, whole.stderr)
            front = aura(["evo", "log", "--front", "--log", str(log)], d, env_for(None))
            self.assertEqual(front.returncode, 0, front.stderr)
            # (2,2) dominates (4,3); the other three are non-dominated.
            self.assertIn("(3 records: 2 KEEP, 1 ROLLBACK)", front.stdout)
            self.assertNotIn("(4 records:", front.stdout)
            chain = aura(["evo", "log", "--lineage", "3", "--log", str(log)], d, env_for(None))
            self.assertEqual(chain.returncode, 0, chain.stderr)
            self.assertIn("(3 records: 2 KEEP, 1 ROLLBACK)", chain.stdout)
            self.assertIn("parent #2", chain.stdout)
            self.assertIn("parent -", chain.stdout)


def _viewbox(svg: str) -> tuple[float, float, float, float]:
    import re

    m = re.search(r'viewBox="([0-9.]+) ([0-9.]+) ([0-9.]+) ([0-9.]+)"', svg)
    assert m, svg[:200]
    return tuple(float(x) for x in m.groups())  # type: ignore[return-value]


class EvoLineageIds(unittest.TestCase):
    """#5069 #5070 #5073 #5074: record-id parents, multi-parent edges,
    unknown --lineage ids, and a header that counts every decision."""

    @staticmethod
    def _row(i: int, parent: str, candidate: str, parents: list[str], decision: str = "KEEP") -> dict:
        return {
            "v": 2,
            "id": f"run#{i}",
            "parent": parent,
            "run": "run",
            "seq": i,
            "ts_ms": 0,
            "kind": "record",
            "target": candidate,
            "decision": decision,
            "reason": "gate",
            "evidence": {"score": float(i)},
            "old": "",
            "new": "",
            "dur_ms": 1.0,
            "algo": "x",
            "gen": i,
            "slot": 0,
            "search": "s",
            "candidate": candidate,
            "parents": parents,
            "behavior": {"x": 0.1, "y": 0.2},
            "objectives": {"loss": float(i), "cost": float(5 - i)},
            "constraints": {},
        }

    def test_multiparent_edges_and_unresolved(self) -> None:
        recs = [
            self._row(1, "", "r1", []),
            self._row(2, "run#1", "r2", ["r1"]),
            self._row(3, "run#2", "r3", ["r1"]),
            self._row(4, "run#3", "r4", ["r2", "r3"]),
            self._row(5, "", "r5", ["missing"], "SCORED"),
        ]
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "log.jsonl"
            log.write_text("".join(json.dumps(x) + "\n" for x in recs))
            out = Path(d) / "report"
            rep = aura(["evo", "report", "--html", str(out), "--log", str(log)], d, env_for(None))
            self.assertEqual(rep.returncode, 0, rep.stderr)
            self.assertIn("did not resolve", rep.stderr)
            tree = (out / "lineage.svg").read_text()
            # r2→r1, r3→r2, r3→r1, r4→r3, r4→r2. r5's "missing" is not an edge.
            self.assertEqual(tree.count("<line "), 5, tree)
            _x, _y, _w, h = _viewbox(tree)
            self.assertGreater(h, 102.0, tree[:120])
            page = (out / "index.html").read_text()
            self.assertIn("5 edges", page)
            self.assertIn("1 unresolved parent link", page)

    def test_unknown_lineage_id_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "log.jsonl"
            log.write_text(json.dumps(self._row(1, "", "r1", [])) + "\n")
            bad = aura(["evo", "log", "--lineage", "nosuchid", "--log", str(log)], d, env_for(None))
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("no record with id 'nosuchid'", bad.stderr)
            self.assertIn("run#1", bad.stderr)
            self.assertNotIn("(no evolution records)", bad.stdout)
            cand = aura(["evo", "log", "--lineage", "c4f27d205", "--log", str(log)], d, env_for(None))
            self.assertNotEqual(cand.returncode, 0, cand.stderr)
            by_name = aura(["evo", "log", "--lineage", "r1", "--log", str(log)], d, env_for(None))
            self.assertEqual(by_name.returncode, 0, by_name.stderr)
            self.assertIn("#1", by_name.stdout)
            by_suffix = aura(["evo", "log", "--lineage", "1", "--log", str(log)], d, env_for(None))
            self.assertEqual(by_suffix.returncode, 0, by_suffix.stderr)
            empty = Path(d) / "empty.jsonl"
            empty.write_text("")
            none = aura(["evo", "log", "--lineage", "nosuchid", "--log", str(empty)], d, env_for(None))
            self.assertEqual(none.returncode, 0, none.stderr)
            self.assertIn("(no evolution records)", none.stdout)

    def test_header_counts_every_decision(self) -> None:
        recs = [
            self._row(1, "", "a", [], "PROPOSE"),
            self._row(2, "run#1", "b", [], "SCORED"),
            self._row(3, "run#2", "b", [], "SCORED"),
            self._row(4, "run#3", "b", [], "KEEP"),
        ]
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "log.jsonl"
            log.write_text("".join(json.dumps(x) + "\n" for x in recs))
            text = aura(["evo", "log", "--log", str(log)], d, env_for(None))
            self.assertEqual(text.returncode, 0, text.stderr)
            self.assertIn("(4 records: 1 PROPOSE, 2 SCORED, 1 KEEP, 0 ROLLBACK)", text.stdout)
            front = aura(["evo", "log", "--front", "--log", str(log)], d, env_for(None))
            self.assertEqual(front.returncode, 0, front.stderr)
            self.assertRegex(front.stdout, r"\(\d+ records: .*\)")
            # The filtered header's counters still add up to the rows shown.
            import re

            m = re.search(r"\((\d+) records: ([^)]*)\)", front.stdout)
            self.assertIsNotNone(m, front.stdout)
            assert m is not None
            total = int(m.group(1))
            parts = [int(p.split()[0]) for p in m.group(2).split(", ") if p.strip()]
            self.assertEqual(sum(parts), total)


class EvoCoreParentChain(unittest.TestCase):
    """#5069: std/evo-core parents are record ids, and the chain survives import."""

    PROG = """
(require "std/evo-core" all:)
(define s (evo:search (hash "algo" "repro" "seed" 1 "budget" 8 "pop" 4
                            "apply" (lambda (s c) #t)
                            "proposer" (lambda (s gen slot rng)
                                         (hash "genome" (list gen slot)
                                               "parents" (if (> gen 0)
                                                             (list "P-A" "P-B")
                                                             (list))
                                               "behavior" (hash "x" 0.5 "y" 0.5))))))
(define c (evo:ask s 2))
(evo:tell! s c (list (hash "objectives" (hash "loss" 1.0 "cost" 1.0)
                           "constraints" (hash "ok" 0.0))
                      (hash "objectives" (hash "loss" 2.0 "cost" 0.5)
                           "constraints" (hash "ok" 0.0))))
(evo:commit! s (car c) (hash "objectives" (hash "loss" 1.0 "cost" 1.0)
                             "constraints" (hash "ok" 0.0)))
(define kid (evo:child (car c) (list 9 9)
                       (hash "parents" (list (evo:candidate-id (car c))
                                             (evo:candidate-id (car (cdr c)))))))
(evo:tell! s (list kid) (list (hash "objectives" (hash "loss" 0.4 "cost" 0.4)
                                    "constraints" (hash "ok" 0.0))))
(display (json-encode (evo:export s)))
(newline)
"""

    RESUME = """
(require "std/evo-core" all:)
(define blob (json-parse (read-file "blob.json")))
(define s (evo:import blob (hash "apply" (lambda (s c) #t)
                                  "proposer" (lambda (s gen slot rng)
                                               (hash "genome" (list "next"))))))
(define c (evo:ask s 1))
(evo:tell! s c (list (hash "objectives" (hash "loss" 0.1 "cost" 0.1)
                           "constraints" (hash "ok" 0.0))))
(display (hash-ref (evo:report s) "tip-record"))
(newline)
"""

    def test_parent_is_record_id_and_survives_restart(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(self.PROG)
            log = Path(d) / "evo.jsonl"
            r = aura(["p.aura"], d, env_for(log))
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            recs = [json.loads(line) for line in log.read_text().splitlines() if line.strip()]
            ids = {x["id"] for x in recs}
            self.assertGreaterEqual(len(recs), 3, recs)
            keeps = [x for x in recs if x["decision"] == "KEEP"]
            self.assertTrue(keeps, recs)
            for x in recs:
                parent = x["parent"]
                self.assertNotEqual(parent, x["id"], x)
                if parent:
                    self.assertIn(parent, ids, x)
                    self.assertNotRegex(parent, r"^c[0-9a-f]+$")
                cand = x.get("candidate") or ""
                if cand:
                    self.assertNotEqual(parent, cand, x)
            self.assertTrue(any(x["parent"] == "" for x in recs))
            # A recombination record names two candidate parents; the report
            # draws both of those edges.
            multi = [x for x in recs if isinstance(x.get("parents"), list) and len(x["parents"]) == 2]
            self.assertTrue(multi, recs)
            last = recs[-1]["id"]
            chain = aura(["evo", "log", "--lineage", last, "--log", str(log)], d, env_for(None))
            self.assertEqual(chain.returncode, 0, chain.stderr)
            self.assertIn(f"({len(recs)} records:", chain.stdout)
            self.assertIn("PROPOSE", chain.stdout)
            self.assertIn("SCORED", chain.stdout)
            self.assertIn("KEEP", chain.stdout)
            self.assertIn("parent -", chain.stdout)
            by_cand = aura(
                ["evo", "log", "--lineage", keeps[-1]["candidate"], "--log", str(log)],
                d,
                env_for(None),
            )
            self.assertEqual(by_cand.returncode, 0, by_cand.stderr)
            self.assertIn("KEEP", by_cand.stdout)
            out = Path(d) / "report"
            rep = aura(["evo", "report", "--html", str(out), "--log", str(log)], d, env_for(None))
            self.assertEqual(rep.returncode, 0, rep.stderr)
            tree = (out / "lineage.svg").read_text()
            # The linear chain plus at least the second recombination parent.
            self.assertGreater(tree.count("<line "), len(recs) - 1, tree)
            _x, _y, w, h = _viewbox(tree)
            self.assertGreater(h, 102.0)
            self.assertLess(w, 17000.0)
            page = (out / "index.html").read_text()
            self.assertIn("edges", page)
            blob = r.stdout.strip().splitlines()[-1]
            self.assertTrue(blob.startswith("{"), r.stdout)
            (Path(d) / "blob.json").write_text(blob)
            (Path(d) / "resume.aura").write_text(self.RESUME)
            r2 = aura(["resume.aura"], d, env_for(log))
            self.assertEqual(r2.returncode, 0, r2.stderr + r2.stdout)
            recs2 = [json.loads(line) for line in log.read_text().splitlines() if line.strip()]
            self.assertGreater(len(recs2), len(recs))
            # The resumed process's first record parents to the exported
            # tip-record, a record id already in the log.
            self.assertEqual(recs2[len(recs)]["parent"], last)


if __name__ == "__main__":
    unittest.main()
