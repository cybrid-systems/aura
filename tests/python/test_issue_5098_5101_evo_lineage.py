#!/usr/bin/env python3
"""Regression for #5098 and #5101: evo lineage is linear in the log.

`aura evo report` walked every node's parent chain with a linear id scan
(cubic on a KEEP chain). `aura evo log --lineage` rescanned the vector at
every parent step (quadratic). A chained log is what the runtime writes:
each record's parent is the previous record's id.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def env() -> dict[str, str]:
    e = dict(os.environ)
    e["AURA_SANDBOX"] = "off"
    e.pop("AURA_PIPELINE_STRICT", None)
    e.pop("AURA_PRESET", None)
    return e


def write_chain(path: Path, n: int) -> str:
    run = "20261110T040601Z-175926"
    prev = ""
    last = ""
    with path.open("w") as f:
        for i in range(n):
            rid = f"{run}#{i + 1}"
            f.write(
                json.dumps(
                    {
                        "v": 2,
                        "id": rid,
                        "parent": prev,
                        "run": run,
                        "seq": i + 1,
                        "ts_ms": 1700000000000 + i,
                        "kind": "record",
                        "target": f"c{i}",
                        "decision": "KEEP",
                        "reason": "why",
                        "evidence": {"score": i},
                        "old": "",
                        "new": "",
                        "dur_ms": 1.0,
                    },
                    separators=(",", ":"),
                )
                + "\n"
            )
            prev = rid
            last = rid
    return last


def run_aura(args: list[str], *, timeout: float) -> tuple[subprocess.CompletedProcess[str], float]:
    t0 = time.perf_counter()
    proc = subprocess.run(
        [str(AURA_BIN), *args],
        capture_output=True,
        text=True,
        env=env(),
        timeout=timeout,
        stdin=subprocess.DEVNULL,
    )
    return proc, time.perf_counter() - t0


class EvoLineageLinear(unittest.TestCase):
    def test_report_chain_is_linear_and_keeps_the_tree(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            small = root / "c2000.jsonl"
            big = root / "c8000.jsonl"
            write_chain(small, 2000)
            write_chain(big, 8000)
            small_dir = root / "s"
            big_dir = root / "b"
            proc_s, t_small = run_aura(["evo", "report", "--log", str(small), "--html", str(small_dir)], timeout=60)
            self.assertEqual(proc_s.returncode, 0, proc_s.stderr)
            proc_b, t_big = run_aura(["evo", "report", "--log", str(big), "--html", str(big_dir)], timeout=60)
            self.assertEqual(proc_b.returncode, 0, proc_b.stderr)
            # Cubic growth is ~64x from 2000 to 8000. Linear is 4x.
            # 12x leaves room for noise and still rejects quadratic (~16x) and cubic.
            self.assertLess(t_big, 2.0, f"8000-record report took {t_big:.3f}s")
            self.assertLess(t_big, max(t_small, 0.05) * 12, f"small={t_small:.3f}s big={t_big:.3f}s")
            tree = (big_dir / "lineage.svg").read_text()
            self.assertEqual(tree.count("<circle "), 8000)
            self.assertEqual(tree.count("<line "), 7999)
            # One chain: the deepest row is the last node, roots sit at y=30.
            self.assertIn('cy="30.0"', tree)
            self.assertIn(f'cy="{(30 + 42 * 7999):.1f}"', tree)

    def test_log_lineage_chain_is_the_full_ancestor_list(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "chain.jsonl"
            last = write_chain(log, 8000)
            proc, elapsed = run_aura(["evo", "log", "--lineage", last, "--log", str(log)], timeout=30)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertLess(elapsed, 0.5, f"8000-record --lineage took {elapsed:.3f}s")
            ids = []
            for line in proc.stdout.splitlines():
                parts = line.split()
                if len(parts) >= 2 and parts[0].startswith("#"):
                    ids.append(parts[0])
            self.assertEqual(ids, [f"#{i}" for i in range(1, 8001)])

    def test_small_chain_lineage_matches_the_old_layout(self) -> None:
        # Three KEEP records, each parent the previous id. Depths 0,1,2;
        # one node per row; edges parent→child. This is the layout the
        # linear depth walk must keep.
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "three.jsonl"
            write_chain(log, 3)
            out = Path(d) / "html"
            proc, _ = run_aura(["evo", "report", "--log", str(log), "--html", str(out)], timeout=30)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            tree = (out / "lineage.svg").read_text()
            self.assertIn('viewBox="0 0 320 186"', tree)
            self.assertIn('<line x1="38.0" y1="30.0" x2="38.0" y2="72.0"', tree)
            self.assertIn('<line x1="38.0" y1="72.0" x2="38.0" y2="114.0"', tree)
            self.assertIn('cy="30.0"', tree)
            self.assertIn('cy="72.0"', tree)
            self.assertIn('cy="114.0"', tree)
            self.assertEqual(tree.count("<circle "), 3)


if __name__ == "__main__":
    unittest.main()
