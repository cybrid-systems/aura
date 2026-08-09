#!/usr/bin/env python3
"""Issue #2822: run_one auto-wires pipeline epoch when TLS unset.

Contract (one row per AC):
  AC1 run_one cites #2822; unset_runs; current_mutation_epoch; base floor
  AC2 always set_pipeline_epoch (no silent skip-only)
  AC3 test suite present
  AC4 linter wired; schema-2822; no docs/design/2822-*; no test_issue_2822.cpp

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

    core = _read("src/compiler/pass_pipeline_core.ixx")
    obs = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    test = _read("tests/compiler/test_run_one_epoch_default.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Definition window (not the forward-decl before run_pipeline).
    pos = core.find("execute a single pass")
    if pos < 0:
        pos = core.rfind("bool run_one")
    body = core[pos : pos + 2800] if pos >= 0 else ""

    # AC1
    must("Issue #2822", "AC1", body)
    must("pipeline_epoch_unset_runs_total", "AC1", body)
    must("current_mutation_epoch", "AC1", body)
    must("kPipelineEpochBaseFloor", "AC1", body)
    must("pipeline_epoch_unset_runs_total", "AC1", core)

    # AC2: always sync (set_pipeline_epoch + sync_total)
    must("set_pipeline_epoch(epoch)", "AC2", body)
    must("pipeline_epoch_sync_total.fetch_add", "AC2", body)
    # Old silent skip must not be the only path in the definition.
    if "if (epoch != 0)" in body and "Issue #2822" not in body:
        fails.append("AC2: residual silent if (epoch != 0) skip without #2822")

    # AC3
    must("ac2822", "AC3", test)
    must("2822", "AC3", test)
    must("pipeline_epoch_hint", "AC3", test)
    must("pipeline_epoch_unset_runs_total", "AC3", test)
    must("run_one", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_run_one_epoch_default.cpp").is_file():
        fails.append("AC3: missing test_run_one_epoch_default.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2822.cpp").is_file():
        fails.append("AC3: test_issue_2822.cpp present (forbidden per #81967)")
    must("test_run_one_epoch_default", "AC3", cmake)

    # AC4
    must("check_run_one_epoch_default_2822", "AC4", build)
    must("schema-2822", "AC4", obs)
    must("pipeline-epoch-unset-runs-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2822-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2822 run_one epoch default auto-wire — no silent skip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
