#!/usr/bin/env python3
"""Cross-mode consistency gate (#4908): one program, one result.

Every program in tests/cross_mode/*.aura runs under
{default, AURA_PIPELINE_STRICT=0, force-soa} x {direct, --load} with
AURA_SANDBOX=off. stdout, the exit code and the first error's line:col must
match the default/direct face; any difference fails with a diff. Intended
differences go in tests/cross_mode/allowlist.txt.

The corpus holds the minimal repros of #4899 #4901 #4903 #4904 (the mode
splits found by the starter kits). Set AURA_STARTER_KITS=<aura-starter-kits
checkout> to also run kits/evolve-strategy (KIT_STEPS, default 20) and compare
its summary.json across the same faces.
"""

from __future__ import annotations

import difflib
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

CORPUS = ROOT / "tests" / "cross_mode"
STRICTS = {"default": None, "strict0": "0", "force-soa": "force-soa"}
FACES = [(s, d) for s in STRICTS for d in ("direct", "load")]
BASE = ("default", "direct")
LOC = re.compile(r"error: (?:[^\s:]+\.aura):(\d+):(\d+):")


def _allowlist() -> list[tuple[str, str, str]]:
    out = []
    for line in (CORPUS / "allowlist.txt").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        prog, face, field = line.split()[:3]
        out.append((prog, face, field))
    return out


def _allowed(allow, prog: str, face: str, field: str) -> bool:
    return any(
        fnmatch.fnmatch(prog, p) and fnmatch.fnmatch(face, f) and fnmatch.fnmatch(field, fl) for p, f, fl in allow
    )


def _env(strict: str | None) -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    for k in ("AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
        env.pop(k, None)
    if strict is not None:
        env["AURA_PIPELINE_STRICT"] = strict
    return env


def _run(path: Path, strict: str | None, mode: str) -> dict[str, str]:
    argv = [str(AURA_BIN)] + (["--load"] if mode == "load" else []) + [str(path)]
    p = subprocess.run(
        argv, capture_output=True, text=True, env=_env(strict), timeout=120, stdin=subprocess.DEVNULL, cwd=path.parent
    )
    m = LOC.search(p.stderr)
    return {
        "stdout": p.stdout,
        "rc": str(p.returncode),
        "loc": f"{m.group(1)}:{m.group(2)}" if m else "-",
        "stderr": p.stderr,
    }


def _diff(prog: str, face: str, field: str, base: str, got: str) -> str:
    d = difflib.unified_diff(
        base.splitlines(keepends=True),
        got.splitlines(keepends=True),
        fromfile=f"{prog} default/direct {field}",
        tofile=f"{prog} {face} {field}",
    )
    return "".join(d) or f"{field}: {base!r} != {got!r}\n"


class CrossModeConsistency(unittest.TestCase):
    def test_corpus_one_result_every_face(self) -> None:
        allow = _allowlist()
        progs = sorted(CORPUS.glob("*.aura"))
        self.assertGreater(len(progs), 0)
        failures = []
        for prog in progs:
            res = {(s, d): _run(prog, STRICTS[s], d) for s, d in FACES}
            base = res[BASE]
            for (s, d), r in res.items():
                face = f"{s}/{d}"
                for field in ("stdout", "rc", "loc"):
                    if r[field] != base[field] and not _allowed(allow, prog.name, face, field):
                        failures.append(_diff(prog.name, face, field, base[field], r[field]) + r["stderr"][-600:])
        self.assertEqual(failures, [], "\n".join(failures))

    @unittest.skipUnless(os.environ.get("AURA_STARTER_KITS"), "set AURA_STARTER_KITS to run the kit leg")
    def test_evolve_strategy_summary_every_face(self) -> None:
        kit = Path(os.environ["AURA_STARTER_KITS"]) / "kits" / "evolve-strategy"
        steps = os.environ.get("KIT_STEPS", "20")
        summaries = {}
        for s, d in FACES:
            with tempfile.TemporaryDirectory() as out:
                env = _env(STRICTS[s]) | {"KIT_STEPS": steps, "KIT_OUT": out}
                argv = [str(AURA_BIN)] + (["--load"] if d == "load" else []) + ["main.aura"]
                p = subprocess.run(
                    argv, cwd=kit, env=env, capture_output=True, text=True, timeout=900, stdin=subprocess.DEVNULL
                )
                self.assertEqual(p.returncode, 0, f"{s}/{d}: {p.stderr[-800:]}")
                summaries[f"{s}/{d}"] = json.loads((Path(out) / "summary.json").read_text())
        base = summaries["default/direct"]
        for face, sm in summaries.items():
            self.assertEqual(sm, base, f"{face}: summary.json differs")


if __name__ == "__main__":
    unittest.main()
