#!/usr/bin/env python3
"""Issue #2768: std/orchestrator agent surface usable on stdin denseness host.

Root cause #2766 (require-before-export free-vars). This issue tracks the
stdlib multi-agent acceptance: agent:spawn/ask/list/status/stop/restart +
orch:registry-epoch + optional orch:parallel-with-yield.

Contract:
  AC1 orchestrator.aura export before require + #2768/#2766 notes
  AC2 ac2768_* spawn/ask/list/epoch + lifecycle tests present
  AC3 host fix cited (#2766) in eval_flat / module loader
  AC4 this linter wired in build.py; no docs/design/2768-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    orch = _read("lib/std/orchestrator.aura")
    t = _read("tests/compiler/test_module_require_freevar.cpp")
    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    loader = _read("src/compiler/evaluator_module_loader.cpp")
    build = _read("build.py")

    # AC1 — export-first stdlib form + notes.
    must("#2768", "AC1", orch)
    must("#2766", "AC1", orch)
    must("agent:spawn", "AC1", orch)
    must("agent:ask", "AC1", orch)
    must("orch:registry-epoch", "AC1", orch)

    # Skip comment lines when locating top-level forms (docs mention require).
    def first_toplevel(prefix: str) -> int:
        pos = 0
        while True:
            i = orch.find(prefix, pos)
            if i < 0:
                return -1
            line_start = orch.rfind("\n", 0, i) + 1
            if i == line_start:  # form at column 0 (not indented comment)
                return i
            pos = i + 1

    exp = first_toplevel("(export ")
    req = first_toplevel("(require ")
    if exp < 0 or req < 0 or exp > req:
        fails.append(f"AC1: export must appear before first require in orchestrator.aura (exp={exp}, req={req})")

    # AC2 — tests.
    must("ac2768_1_orchestrator_export_first", "AC2", t)
    must("ac2768_2_spawn_ask_list_epoch", "AC2", t)
    must("ac2768_3_status_stop_restart", "AC2", t)
    must("ac2768_4_parallel_with_yield_smoke", "AC2", t)
    must("ac2768_5_linter", "AC2", t)
    must("agent:status", "AC2", t)
    must("agent:restart", "AC2", t)
    must("orch:registry-epoch", "AC2", t)

    # AC3 — host fix still present (#2766).
    must("#2766", "AC3", efl)
    must("is_module_prologue", "AC3", efl)
    must("#2766", "AC3", loader)
    must("set_pool", "AC3", loader)

    # AC4 — linter wire.
    must("check_orchestrator_agent_stdin_2768", "AC4", build)
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("2768-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2768 std/orchestrator agent stdin denseness surface — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
