#!/usr/bin/env python3
"""Issue #3452: mutate:* registration is MutateRegKind, not audit-header concept.

typed_mutation_audit.h is trail/strategy. I4 compile-time lives in
mutate_dispatch.hh MutateRegKind + the #3074 linter. Raw add("mutate:
must be MetadataGuardExempt + GUARD_EXEMPT. Structural prims stay
add_mutate. Soft/Off: registration-time only. No new query key.

Contract:
  AC1 raw add("mutate: that is not EXEMPT fails #3074 (count == 0)
  AC2 structural list still registers through add_mutate
  AC3 EXEMPT metadata prims still skip acquire
  AC4 audit header documents SSOT pointer; MutateRegKind in dispatch
  AC5 no docs/design/3452-*; no test_issue_3452.cpp; no new query key

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    disp = _read("src/compiler/mutate_dispatch.hh")
    audit = _read("src/compiler/typed_mutation_audit.h")
    l3074 = _read("scripts/coverage/checks/check_mutate_dispatch_sole_guard_3074.py")
    t = _read("tests/compiler/test_mutation_guard_try_acquire_unit.cpp")
    build = _read("build.py")

    lines = mut.splitlines()
    for i, ln in enumerate(lines, 1):
        s = ln.lstrip()
        if s.startswith("//") or s.startswith("*"):
            continue
        if 'add("mutate:' not in ln:
            continue
        win = "\n".join(lines[max(0, i - 12) : i + 16])
        if "GUARD_EXEMPT" not in win or "MetadataGuardExempt" not in win:
            fails.append(f'AC1: raw add("mutate: at mutate.cpp:{i} is not EXEMPT')
    must("AC3452: raw add", "AC1 3074 extra", l3074)
    must("ac3452_1_raw_add_exempt_only", "AC1 test", t)

    for name in (
        "mutate:replace-type",
        "mutate:replace-value",
        "mutate:replace-pattern",
        "mutate:replace-subtree",
        "mutate:rename-symbol",
        "mutate:move-node",
        "mutate:atomic-batch",
        "mutate:query-and-replace",
        "mutate:set-body",
        "mutate:rebind",
    ):
        idx = 0
        via = False
        while True:
            pos = mut.find(f'"{name}"', idx)
            if pos < 0:
                break
            look = mut[max(0, pos - 200) : pos]
            if "add_mutate" in look:
                via = True
                break
            idx = pos + 1
        if idx == 0 and not via:
            fails.append(f"AC2: missing {name}")
        elif not via:
            fails.append(f"AC2: {name} is not through add_mutate")
    must("ac3452_2_structural_add_mutate", "AC2 test", t)

    must("/*guard_exempt=*/true", "AC3 exempt flag", mut)
    must("mutate:set-agent-fingerprint", "AC3 fingerprint", mut)
    must("ac3452_3_exempt_skip_acquire", "AC3 test", t)

    must("enum class MutateRegKind", "AC4 enum", disp)
    must("kMutateRegKindIssue = 3452", "AC4 stamp", disp)
    must("MutateRegKind", "AC4 audit cite", audit)
    must("check_mutate_dispatch_sole_guard_3074.py", "AC4 audit SSOT", audit)
    must("ac3452_4_audit_header_ssot", "AC4 test", t)
    if "schema-3452" in mut or "schema-3452" in disp or "schema-3452" in audit:
        fails.append("AC4: new schema-3452 query key")

    must("check_mutate_reg_kind_3452", "AC5 build.py", build)
    must("ac3452_5_source_and_linter", "AC5 test", t)
    prev = build.find("check_mutate_dispatch_sole_guard_3074")
    ours = build.find("check_mutate_reg_kind_3452")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3452 linter must be wired after #3074")
    if (ROOT / "tests" / "compiler" / "test_issue_3452.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3452.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3452.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3452.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3452-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3452 mutate_reg_kind:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3452 mutate_reg_kind: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
