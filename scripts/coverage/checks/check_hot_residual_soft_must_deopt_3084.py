#!/usr/bin/env python3
"""Issue #3084: Soft residual non-identity CastOp must force deopt.

Production still density-keep + force-JIT/relower (#3046). Soft leftover>0
marks MustDeopt / force-deopt (no relower). leftover==0 is Quiet.
Blame-complete default stays Soft observe-only.

Contract (one row per AC):
  AC1 Soft leftover>0 → MustDeopt pending + observe counter
  AC2 Production keep + relower unchanged
  AC3 leftover==0 → zero extra
  AC4 require_blame_complete_on_commit default unchanged
  AC5 extend test_castop_density_hard; this linter; no invent / no design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    pol = _read("src/compiler/castop_density_policy.hh")
    opt = _read("src/compiler/optimization_passes.ixx")
    blame = _read("src/compiler/coercion_provenance_policy.hh")
    q = read_query_prims()
    dens = _read("tests/compiler/test_castop_density_hard.cpp")
    dce = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    build = _read("build.py")

    must("kCastOpHotResidualSoftMustDeoptIssue = 3084", "AC1 stamp", pol)
    must("g_hot_residual_soft_must_deopt_total", "AC1 total", pol)
    must("g_hot_residual_soft_must_deopt_pending", "AC1 pending", pol)
    must("Issue #3084", "AC1 policy", pol)
    must("Issue #3084", "AC1 sweep", opt)
    must("note_hot_residual_nonidentity_castops", "AC1 note", opt)
    must("ac3084_1_soft_residual_must_deopt", "AC1 test", dens)
    if "aura_jit_batch_deopt_for" not in pol and "on_stale_deopt" not in pol:
        fails.append("AC1: Soft path missing force-deopt equivalent")

    must("on_force_jit_for_reason", "AC2 Production relower", pol)
    must("g_hot_residual_density_keep_total", "AC2 keep", pol)
    must("g_hot_residual_relower_total", "AC2 relower", pol)
    must("ac3084_2_production_relower_unchanged", "AC2 test", dens)
    must("ac3046_residual_nonidentity", "AC2 #3046 retained", dens)

    must("leftover == 0", "AC3 Quiet", pol)
    must("ac3084_3_quiet_zero", "AC3 test", dens)
    must("Soft / identity path: zero extra", "AC3 sweep Quiet", opt)

    must("require_blame_complete_on_commit = false (#2221 observe-only)", "AC4 default", blame)
    must("require_blame_complete_on_commit = true under Restricted/Strict", "AC4 Restricted", blame)
    must("ac3084_4_blame_default_unchanged", "AC4 test", dens)
    if "require_blame_complete_on_commit" in pol:
        fails.append("AC4: residual helper must not flip blame policy")

    must_key("schema-3084", "AC5 schema", q)
    must_key("hot-residual-soft-must-deopt-wired", "AC5 wired", q)
    must("schema-3046", "AC5 lineage 3046", q)
    must("ac3084_5_schema_and_linter", "AC5 test", dens)
    must("check_hot_residual_soft_must_deopt_3084", "AC5 build.py", build)
    must("sweep_production_hot_residual_castops", "AC5 DCE sweep", dce)
    if (ROOT / "tests" / "compiler" / "test_issue_3084.cpp").is_file():
        fails.append("AC5: test_issue_3084.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3084-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3084 Soft residual CastOp MustDeopt — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
