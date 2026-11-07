#!/usr/bin/env python3
"""Issue #2646: cone-truncate must drop goals/memo for dirty Ifs outside truncated cone.

Contract:
  AC1 Soft + cone soft overflow + dirty If outside cone → goals dropped (commit allowed)
  AC2 production + same → commit hard + outside goals dropped
  AC3 !truncated path → counters do not advance; empty outside set → no extra work
  AC4 #2622 diverge metric ordering — outside invalidate fires AFTER #2622 sync
  AC5 Additive schema + linter + build.py wire (source-cite #2621 + #2622 + #2646)
  AC6 Unit test fixture deferred — full soak requires drift-injection helper

Exit 0 = all AC rows satisfied.
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


def _function_body(text: str, signature: str) -> str:
    """Brace-matched body of the first function whose header contains signature.

    Comments and string/character literals do not move the brace depth, so a
    comment that mentions another function cannot steal the span.
    """
    start = text.find(signature)
    if start < 0:
        return ""
    i = text.find("{", start)
    if i < 0:
        return ""
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if c in "\"'":
            q = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    return ""


def _outside_after_sync(blame_body: str) -> str | None:
    """#2646 outside-cone invalidate must follow the #2622 memo sync in this body."""
    sync = blame_body.find("sync_occurrence_after_dirty(")
    memo = blame_body.find("memo_targets.data()", sync if sync >= 0 else 0)
    mark = blame_body.find("Issue #2646: cone-truncate must drop goals/memo")
    gate = blame_body.find("if (st.tc.last_partial_cone_truncated_)")
    nonempty = blame_body.find("if (!outside_cone_conds.empty())")
    second = blame_body.find("outside_cone_conds.data()")
    if sync < 0 or memo < 0 or not (sync < memo < sync + 600):
        return "missing #2622 sync_occurrence_after_dirty(memo_targets)"
    if mark < 0 or gate < 0 or nonempty < 0 or second < 0:
        return "missing #2646 truncated gate, empty short-circuit, or outside sync"
    if not (memo < mark < gate < nonempty < second):
        return "#2646 outside-invalidate is not after the #2622 memo sync"
    return None


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    impl = _read("src/compiler/type_checker_impl.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    test = _read("tests/compiler/test_partial_cone_commit_gate.cpp")
    build = _read("build.py")

    # AC1+AC2: wiring present
    must("Issue #2646: cone-truncate must drop goals/memo", "AC1", impl)
    must("outside_cone_conds", "AC1", impl)
    must("engine.sync_occurrence_after_dirty", "AC1", impl)
    must("last_partial_cone_truncated_", "AC2", impl)

    # AC3+AC4 are scoped to the helper that owns the invalidate, and to the
    # orchestrator that runs cone truncate (which stores the flag) before it.
    # A file-wide string hit is not enough: the old `if (last_partial_cone_truncated_)`
    # shape is a substring of the new one and would pass even if the gate moved.
    blame = _function_body(impl, "void partial_blame_and_selective_invalidate(")
    trunc = _function_body(impl, "void partial_truncate_cone_and_invalidate(")
    # The one-argument overload only forwards. The orchestration lives on the
    # extra_recs overload.
    orch = _function_body(impl, "std::span<const aura::ast::MutationRecord> extra_recs)")
    if not blame:
        fails.append("AC3: partial_blame_and_selective_invalidate body missing")
    else:
        why = _outside_after_sync(blame)
        if why:
            fails.append(f"AC3/AC4: {why}")
        if "outside_cone_conds.empty()" not in blame:
            fails.append("AC3: empty outside set is not the short-circuit inside the blame helper")
    if "st.tc.last_partial_cone_truncated_ = truncated;" not in trunc:
        fails.append("AC3: truncate helper no longer stores last_partial_cone_truncated_")
    trunc_call = orch.find("partial_truncate_cone_and_invalidate(st)")
    blame_call = orch.find("partial_blame_and_selective_invalidate(st, engine)")
    if trunc_call < 0 or blame_call < 0 or not (trunc_call < blame_call):
        fails.append(
            "AC4: infer_flat_partial must call partial_truncate_cone_and_invalidate "
            "before partial_blame_and_selective_invalidate"
        )
    # Counterexample: the same predicate must reject a body that runs the
    # outside-cone sync before the #2622 memo sync, and one that drops the gate.
    # The marker is placed before the memo sync on purpose.
    reversed_body = (
        "// Issue #2646: cone-truncate must drop goals/memo\n"
        "if (st.tc.last_partial_cone_truncated_) {\n"
        "  if (!outside_cone_conds.empty()) sync(outside_cone_conds.data());\n"
        "}\n"
        "sync_occurrence_after_dirty(memo_targets.data());\n"
    )
    if _outside_after_sync(reversed_body) is None:
        fails.append("AC4 negative: reversed #2646-before-#2622 body was accepted")
    ungated = blame.replace("if (st.tc.last_partial_cone_truncated_)", "if (true)", 1)
    if blame and _outside_after_sync(ungated) is None:
        fails.append("AC3 negative: dropping the truncated gate was accepted")

    # AC5: source-cite + counters + linter registration
    must("#2646", "AC5", impl)
    must("#2621", "AC5", impl)
    must("#2622", "AC5", impl)
    must("occurrence_cone_outside_invalidate_total", "AC5", obs)
    must("occurrence_cone_outside_goals_dropped_total", "AC5", obs)
    must("occurrence_cone_outside_memo_dropped_total", "AC5", obs)
    must("occurrence_cone_outside_invalidate_total", "AC5", fields)
    must("occurrence_cone_outside_goals_dropped_total", "AC5", fields)
    must("occurrence_cone_outside_memo_dropped_total", "AC5", fields)
    must("#2646", "AC5", test)
    must("ac2646_outside_cone_invalidate_source_cite", "AC5", test)
    must("check_occurrence_cone_outside_invalidate_2646", "AC5", build)

    # AC6: no docs/design — verify no docs/design/2646-* file exists
    design_dir = ROOT / "docs" / "design"
    design_files = list(design_dir.glob("2646*")) if design_dir.exists() else []
    if design_files:
        fails.append(f"AC6: docs/design/2646-* exists ({len(design_files)} files)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2646 cone-truncate outside-cone invalidate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
