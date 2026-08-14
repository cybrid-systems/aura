#!/usr/bin/env python3
"""Issue #3046: session-mid always stamps under hf mutate; residual
non-identity CastOp never silent-JITs under Production.

Contract:
  AC1 session mid always stamps (incl. weak leftover); no pre-mutate blame
  AC2 residual non-identity CastOp → density-policy keep / force-relower
  AC3 Soft / identity / empty leftover → zero extra
  AC4 schema-3046 + extend test_coercion_stamp_at_add / DeadCoercion
  AC5 source cites coercion_map + castop_density_policy; no docs/design/
  AC6 this linter wired in build.py; no test_issue_3046.cpp

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

    cm = _read("src/compiler/coercion_map.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    pol = _read("src/compiler/castop_density_policy.hh")
    opt = _read("src/compiler/optimization_passes.ixx")
    q = read_query_prims()
    stamp = _read("tests/compiler/test_coercion_stamp_at_add.cpp")
    dce = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    dens = _read("tests/compiler/test_castop_density_hard.cpp")
    build = _read("build.py")

    # AC1
    must("kCoercionBlameHfLagIssue = 3046", "AC1", cm)
    must("g_coercion_blame_session_force_total", "AC1", cm)
    must("non-zero session always stamps", "AC1", cm)
    must("g_coercion_blame_stale_narrowing_drop_total", "AC1", impl)
    must("ac3046_1_session_always_stamps_weak", "AC1", stamp)

    # AC2
    must("note_hot_residual_nonidentity_castops", "AC2", pol)
    must("note_hot_residual_nonidentity_castops", "AC2", opt)
    must("count_all_castops", "AC2", opt)
    must("g_hot_residual_density_keep_total", "AC2", pol)
    must("ac3046_residual_nonidentity", "AC2", dens)
    must("ac3046_nonidentity_density_cite", "AC2", dce)

    # AC3
    must("session == 0", "AC3", cm)
    must("leftover == 0", "AC3", pol)
    must("ac3046_3_quiet_no_session", "AC3", stamp)
    must("Soft / identity path: zero extra", "AC3", opt)

    # AC4
    must("schema-3046", "AC4", q)
    must("coercion-blame-session-force-total", "AC4", q)
    must("hot-residual-nonidentity-total", "AC4", q)
    must("ac3046_4_schema", "AC4", stamp)

    # AC5
    must("#3046", "AC5 coercion_map", cm)
    must("#3046", "AC5 castop_density_policy", pol)
    must("ac3046_5_source_cites", "AC5", stamp)

    # AC6
    must("check_coercion_hf_lag_hot_residual_3046", "AC6", build)
    must("cmd_coercion_hf_lag_hot_residual_3046_coverage", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3046.cpp").is_file():
        fails.append("tests/compiler/test_issue_3046.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*3046*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3046 session-mid always-stamp + residual CastOp density keep")
    return 0


if __name__ == "__main__":
    sys.exit(main())
