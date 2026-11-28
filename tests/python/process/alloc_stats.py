"""Issue #5636: alloc-stats capacity tracks RSS, and each subsystem moves."""

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


def _run(source: str, *, timeout: float = 120) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    env["AURA_PATH"] = str(ROOT / "lib")
    env.pop("AURA_ALLOC_TRACE", None)
    try:
        return subprocess.run(
            [str(AURA_BIN), path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
    finally:
        os.unlink(path)


def _json_lines(stdout: str) -> list[dict]:
    found = []
    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            found.append(json.loads(line))
    return found


def _sub(doc: dict, name: str, field: str) -> int:
    return int(doc["subsystems"][name][field])


_MOVE = r"""
(begin
  (display (stats:get "engine:alloc-stats"))
  (newline)
  (define i 0)
  (define xs '())
  (while (< i 8000)
    (begin
      (set! xs (cons i xs))
      (set! i (+ i 1))))
  (define j 0)
  (while (< j 2000)
    (begin
      (string-append "uniq-" (number->string j) "-pad-value")
      (set! j (+ j 1))))
  (define k 0)
  (while (< k 200)
    (begin
      (eval (string-append "(define cellv" (number->string k) " " (number->string k) ")"))
      (set! k (+ k 1))))
  (fiber:join (fiber:spawn (lambda () (string-append "nursery-bytes" "xx"))))
  (display (stats:get "engine:alloc-stats"))
  (newline))
"""


class AllocStats(unittest.TestCase):
    before: dict
    after: dict

    @classmethod
    def setUpClass(cls) -> None:
        if not AURA_BIN.exists():
            raise unittest.SkipTest(f"{AURA_BIN} not built")
        proc = _run(_MOVE, timeout=180)
        if proc.returncode != 0:
            raise AssertionError(proc.stderr[-2000:] + proc.stdout[-500:])
        docs = _json_lines(proc.stdout)
        if len(docs) < 2:
            raise AssertionError(proc.stdout[-2000:])
        cls.before = docs[0]
        cls.after = docs[-1]

    def test_default_cli_reads_stats(self) -> None:
        proc = _run(
            "(begin"
            ' (display (stats:get "arena:stats-json")) (newline)'
            ' (display (engine:metrics "arena:stats-json")) (newline)'
            ' (display (stats:get "engine:alloc-stats")) (newline)'
            ' (display (engine:metrics "engine:alloc-stats")) (newline))'
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-1500:])
        docs = _json_lines(proc.stdout)
        self.assertGreaterEqual(len(docs), 2, proc.stdout[-1500:])
        self.assertIn("arenas", proc.stdout)
        rss_docs = [d for d in docs if "rss_bytes" in d]
        self.assertGreaterEqual(len(rss_docs), 2, proc.stdout[-1500:])
        for doc in rss_docs:
            self.assertIn("capacity_bytes", doc)
            self.assertIn("minor_faults", doc)
            self.assertIn("ast_arena", doc["subsystems"])

    def test_capacity_tracks_rss_after_std_load(self) -> None:
        lines = []
        for path in sorted((ROOT / "lib" / "std").rglob("*.aura")):
            if path.name == "bus_agent_cli.aura":
                continue
            rel = path.relative_to(ROOT / "lib").with_suffix("")
            lines.append(f'(require "{rel.as_posix()}" all:)')
        lines.append('(display (stats:get "engine:alloc-stats"))')
        lines.append("(newline)")
        proc = _run("\n".join(lines) + "\n", timeout=180)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        docs = _json_lines(proc.stdout)
        self.assertGreaterEqual(len(docs), 1, proc.stdout[-1500:])
        doc = docs[-1]
        rss = int(doc["rss_bytes"])
        cap = int(doc["capacity_bytes"])
        self.assertGreater(rss, 50 * 1024 * 1024, doc)
        self.assertLessEqual(abs(cap - rss) / rss, 0.10, doc)
        sub = doc["subsystems"]
        rolled = (
            int(sub["process_image"]["capacity"])
            + int(sub["ast_arena"]["capacity"])
            + int(sub["runtime_heap"]["capacity"])
        )
        self.assertEqual(rolled, cap)
        self.assertGreater(int(sub["ast_arena"]["capacity"]), 1024, sub["ast_arena"])
        self.assertGreater(int(sub["process_image"]["capacity"]), 1024 * 1024)

    def test_ast_arena_grows(self) -> None:
        # Defines live in FlatAST. Loading a module builds more arenas and
        # touches their slabs, which is what resident_bytes reports.
        tiny = _run('(display (stats:get "engine:alloc-stats"))\n(newline)\n')
        self.assertEqual(tiny.returncode, 0, tiny.stderr[-800:])
        big = _run(
            '(require "std/list" all:)\n(display (stats:get "engine:alloc-stats"))\n(newline)\n',
            timeout=60,
        )
        self.assertEqual(big.returncode, 0, big.stderr[-800:])
        a = _sub(_json_lines(tiny.stdout)[-1], "ast_arena", "capacity")
        b = _sub(_json_lines(big.stdout)[-1], "ast_arena", "capacity")
        self.assertGreater(b, a)

    def test_flat_ast_grows(self) -> None:
        self.assertGreater(
            _sub(self.after, "flat_ast_columns", "used"),
            _sub(self.before, "flat_ast_columns", "used"),
        )

    def test_pcv_grows(self) -> None:
        self.assertGreater(
            _sub(self.after, "pcv_heap", "used"),
            _sub(self.before, "pcv_heap", "used"),
        )

    def test_ir_grows(self) -> None:
        # The tree walker does not lower. (eval-current :jit) does.
        before = _run('(display (stats:get "engine:alloc-stats"))\n(newline)\n')
        self.assertEqual(before.returncode, 0, before.stderr[-800:])
        after = _run(
            '(begin (define (f x) (+ x 1)) (eval-current :jit) (display (stats:get "engine:alloc-stats")) (newline))\n',
            timeout=60,
        )
        self.assertEqual(after.returncode, 0, after.stderr[-800:])
        self.assertGreater(
            _sub(_json_lines(after.stdout)[-1], "ir_soa_slab", "used"),
            _sub(_json_lines(before.stdout)[-1], "ir_soa_slab", "used"),
        )

    def test_pairs_grow(self) -> None:
        self.assertGreater(
            _sub(self.after, "runtime_pairs", "used"),
            _sub(self.before, "runtime_pairs", "used"),
        )

    def test_cells_grow(self) -> None:
        self.assertGreater(
            _sub(self.after, "runtime_cells", "used"),
            _sub(self.before, "runtime_cells", "used"),
        )

    def test_strings_grow(self) -> None:
        self.assertGreater(
            _sub(self.after, "string_heap", "used"),
            _sub(self.before, "string_heap", "used"),
        )

    def test_fiber_grows(self) -> None:
        self.assertGreater(
            _sub(self.after, "fiber_nursery", "used"),
            _sub(self.before, "fiber_nursery", "used"),
        )

    def test_name_res_grows(self) -> None:
        self.assertGreater(
            _sub(self.after, "name_res_by_node", "used"),
            _sub(self.before, "name_res_by_node", "used"),
        )
