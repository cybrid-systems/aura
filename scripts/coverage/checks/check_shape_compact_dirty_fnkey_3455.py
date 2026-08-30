#!/usr/bin/env python3
"""Issue #3455: on_arena_compact versions dirty ∪ relocated FnKeys only.

#2617 closed storm-ring feed. #3199 closed all-shards unique. Residual:
compact still walked every profile under each shard lock, so a Moving
auto-arm after a one-fn mutate deopted the whole shape table.

Contract:
  AC1 Production compact after mutate of F: hooks / deopt_from_arena_compact
      delta == dirty∪relocated count, not tracked_fns(). Untouched G keeps
      version + is_stable.
  AC2 compact still does not call update_deopt_storm_state_ / grow
      deopt_ring_count_ (#2617). deopt_storm_compact_suppressed still bumps.
  AC3 PerEval: process-global shape_version still not advanced from
      compact-only (#2908).
  AC4 Shard locking stays one-shard-at-a-time (#3199).
  AC5 no docs/design/3455-*; no test_issue_3455.cpp; no new query key.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _extract_fn_body(src: str, sig_pat: str) -> str | None:
    m = re.search(sig_pat, src)
    if not m:
        return None
    i = src.find("{", m.end() - 1)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i : j + 1]
    return None


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/shape_profiler.h")
    cpp = _read("src/compiler/shape_profiler.cpp")
    svc = _read("src/compiler/service.ixx")
    iso = _read("tests/compiler/test_shape_compact_storm_isolation.cpp")
    conc = _read("tests/compiler/test_shape_profiler_concurrency.cpp")
    l2617 = _read("scripts/coverage/checks/check_shape_compact_storm_isolation_2617.py")
    l3199 = _read("scripts/coverage/checks/check_shape_compact_no_all_shards_lock_3199.py")
    l2908 = _read("scripts/coverage/checks/check_shape_compact_no_global_bump_2908.py")
    build = _read("build.py")

    must("kShapeCompactDirtyFnkeyIssue = 3455", "AC1 stamp", hh)
    must("dirty_or_relocated", "AC1 header span", hh)
    must("Issue #3455", "AC1 cpp", cpp)
    must("any_block_dirty", "AC1 service dirty mask", svc)
    must("make_fn_key(session_id_", "AC1 service FnKey", svc)

    compact = _extract_fn_body(cpp, r"ShapeProfiler::on_arena_compact\s*\(")
    if not compact:
        fails.append("AC1: could not extract on_arena_compact")
    else:
        stripped = re.sub(r"//[^\n]*", "", compact)
        must("dirty_or_relocated", "AC1 span param", compact)
        must("profiles.find(fn)", "AC1 cone find", compact)
        if "for (auto&& [fn, profile] : profiles)" in stripped:
            fails.append("AC1: on_arena_compact still walks every profile")
        if re.search(r"\bupdate_deopt_storm_state_\s*\(", stripped):
            fails.append("AC2: on_arena_compact calls update_deopt_storm_state_")
        must("Explicitly do NOT call update_deopt_storm_state_", "AC2", compact)
        must("deopt_storm_compact_suppressed", "AC2 suppressed", compact)
        if re.search(r"\bunique_lock_all_shards_\s*\(", stripped):
            fails.append("AC4: on_arena_compact calls unique_lock_all_shards_")
        must("unique_lock_shard_", "AC4 per-shard", compact)
        must("allow_global_version_bump", "AC3 PerEval gate", compact)

    must("ac3455_compact_dirty_cone_only", "AC1 test", iso)
    must("deopt_from_arena_compact_total", "AC1 counter", iso)
    must("check_shape_compact_storm_isolation_2617", "AC2 lineage", l2617)
    must("check_shape_compact_no_all_shards_lock_3199", "AC4 lineage", l3199)
    must("check_shape_compact_no_global_bump_2908", "AC3 lineage", l2908)
    must("ac3199_2_version_advances", "AC4 concurrency suite", conc)

    must("check_shape_compact_dirty_fnkey_3455", "AC5 build.py", build)
    if "schema-3455" in hh or "schema-3455" in cpp or "schema-3455" in svc:
        fails.append("AC5: new schema-3455 query key")
    if (ROOT / "tests" / "compiler" / "test_issue_3455.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3455.cpp")
    if (ROOT / "tests" / "core" / "test_issue_3455.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3455.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3455-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3455 compact dirty∪relocated FnKey filter — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
