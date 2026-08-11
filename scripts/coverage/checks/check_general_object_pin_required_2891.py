#!/usr/bin/env python3
"""Issue #2891: Force wire_general_object_create_pair_or_required_fail
return-value check on all intermediate create hotpaths (eliminate void-cast
residual).

Residual of #2840/#2709: the helper + sticky breach exist, but some
mutate/agent/scratch intermediate create sites still void-cast the return
value (or skip the wire entirely), so a pin failure under production
required mode only sets sticky densify-off without failing the create
path. #2891:
  AC1 set-code + check-form intermediate creates use
      wire_general_object_create_pair_or_required_fail and fail closed
      under production required (no void-cast, no ignored return)
  AC2 Soft / required off stays observe-only (helper returns true)
  AC3 existing auto-wire + adopt counters preserved; no new atomics on
      quiet path
  AC4 linter fails on any remaining void-cast / bare wire of the
      required-fail helper in mutate/agent/scratch create paths
  AC5 source-cite + extend src/-aligned general-object-pin suite per
      #81967; no docs/design/ per #1655

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

    lp = _read("src/core/lifetime_pin.hh")
    ev = _read("src/compiler/evaluator_primitives_eval.cpp")
    tst = _read("src/compiler/evaluator_primitives_test.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    qw = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    build = _read("build.py")

    # AC1 — set-code intermediate create uses required-fail wire + fail closed
    must("Issue #2891", "AC1", ev)
    must("wire_general_object_create_pair_or_required_fail", "AC1", ev)
    must("set-code: GeneralObjectPin required under production (#2891)", "AC1", ev)
    # check-form scratch create (temp_arena_) wires + fails clause closed
    must("Issue #2891", "AC1", tst)
    must("wire_general_object_create_pair_or_required_fail", "AC1", tst)
    must("import aura.core.lifetime_pin", "AC1", tst)

    # AC1b — all mutate/agent/scratch create paths already use required-fail
    # (regression guard: the 7 wired sites from #2840/#2363 must stay wired)
    for fname, f in (("mutate", mut), ("query_workspace", qw), ("eval_flat", flat)):
        if "wire_general_object_create_pair_or_required_fail" not in f:
            fails.append(f"AC1b: {fname} lost required-fail wire")

    # AC2 — Soft observe-only preserved in helper
    must("Soft: observe-only", "AC2", lp)
    must("return true;   // Soft: observe-only", "AC2", lp)

    # AC3 — existing auto-wire + adopt counters preserved, quiet path cheap
    must("general_object_pin_auto_wire_total", "AC3", lp)
    must("kGeneralObjectPinAdoptSiteCount", "AC3", lp)

    # AC4 — no void-cast / bare wire of the helper in create paths
    for fname, f in (
        ("evaluator_primitives_eval.cpp", ev),
        ("evaluator_primitives_test.cpp", tst),
        ("evaluator_primitives_mutate.cpp", mut),
        ("evaluator_primitives_query_workspace.cpp", qw),
        ("evaluator_eval_flat.cpp", flat),
    ):
        if "(void)aura::core::lifetime::wire_general_object_create_pair_or_required_fail" in f:
            fails.append(f"AC4: {fname} void-casts or_required_fail helper")
        # bare wire_general_object_create_pair( without the required-fail
        # suffix on a create path is a residual (must route through helper).
        # Allow the declaration / comments / helper internals in lifetime_pin.
        for line in f.splitlines():
            s = line.strip()
            if s.startswith("wire_general_object_create_pair(") or (
                "wire_general_object_create_pair(" in s
                and "or_required_fail" not in s
                and "or_exempt" not in s
                and "//" not in s
                and not s.startswith("using ")
                and "::wire_general_object_create_pair("
                not in s.replace("wire_general_object_create_pair_or_required_fail", "")
            ):
                fails.append(f"AC4: {fname} bare wire_general_object_create_pair call")

    # AC5 — test extension + linter wiring + no docs/design/invent
    must("ac2891_1_set_code_and_check_form_wire", "AC5", test)
    must("ac2891_2_soft_observe_only", "AC5", test)
    must("ac2891_3_no_void_cast_linter", "AC5", test)
    must("check_general_object_pin_required_2891", "AC5", build)
    if (ROOT / "tests" / "core" / "test_issue_2891.cpp").is_file():
        fails.append("AC5: test_issue_2891.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2891-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2891 required-fail return check on intermediate create hotpaths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
