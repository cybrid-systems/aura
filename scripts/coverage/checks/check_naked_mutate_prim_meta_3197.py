#!/usr/bin/env python3
"""Issue #3197: PrimMeta / registration-time naked-mutate hard gate.

add_mutate already fail-closes at runtime after a naked body. This
issue stamps PrimMeta.requires_mutation_guard at registration and
requires every non-GUARD_EXEMPT mutate:* body to acquire via
mutate_dispatch_try_acquire (or run_under_mutation_guard).

Contract:
  AC1 non-exempt add_mutate stamps PrimMeta.requires_mutation_guard
  AC2 production naked body (no wrap bump AND no acquire token) →
      structured naked-mutate + mark_outermost_mutation_failed
  AC3 linter: non-exempt mutate:* without acquire / thin wrapper fails
  AC4 Soft: naked_mutate_attempt only
  AC5 extend mutation-guard suite; no public query key; no invent / docs

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


ADD_MUTATE_RE = re.compile(r'add_mutate\(\s*"([^"]+)"', re.MULTILINE)


def _scan_body(hay: str, name: str) -> tuple[str, str]:
    m = re.search(r'add_mutate\(\s*"' + re.escape(name) + r'"', hay)
    if not m:
        return ("", "")
    br = hay.find("[", m.end())
    if br < 0:
        return ("", "")
    brace = hay.find("{", br)
    if brace < 0:
        return ("", "")
    depth = 1
    i = brace + 1
    while i < len(hay) and depth:
        if hay[i] == "{":
            depth += 1
        elif hay[i] == "}":
            depth -= 1
        i += 1
    return hay[brace:i], hay[i : i + 160]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    disp = _read("src/compiler/mutate_dispatch.hh")
    scaf = _read("src/compiler/prim_registrar_scaffold.hh")
    t = _read("tests/compiler/test_mutation_guard_try_acquire_unit.cpp")
    build = _read("build.py")

    # AC1
    must("requires_mutation_guard", "AC1 PrimMeta", ixx)
    must("requires_mutation_guard = !guard_exempt", "AC1 add_mutate stamp", mut)
    must("requires_mutation_guard", "AC1 PrimSpec", scaf)
    must("kNakedMutatePrimMetaIssue = 3197", "AC1 stamp", disp)
    must("ac3197_1_prim_meta_requires_guard", "AC1 test", t)

    # AC2
    must("mutate_guard_acquire_token", "AC2 token", mut)
    must("note_mutate_guard_acquire_token", "AC2 note", disp)
    must("naked-mutate", "AC2 kind", mut)
    must("mark_outermost_mutation_failed", "AC2 mark-failed", mut)
    must("ac3197_2_token_and_fail_closed", "AC2 test", t)

    # AC3 — every non-exempt add_mutate has acquire or thin wrapper
    names = ADD_MUTATE_RE.findall(mut)
    if not names:
        fails.append("AC3: no add_mutate registrations found")
    for name in names:
        body, after = _scan_body(mut, name)
        if not body:
            fails.append(f"AC3: could not parse body for {name}")
            continue
        exempt = "guard_exempt" in after
        has = "mutate_dispatch_try_acquire" in body or "run_under_mutation_guard" in body
        if exempt:
            continue
        if not has:
            fails.append(
                f"AC3: {name} is not GUARD_EXEMPT and missing mutate_dispatch_try_acquire / run_under_mutation_guard"
            )

    # AC4
    must("naked_mutate_attempt", "AC4 observe", mut)
    must("ac3197_4_soft_observe", "AC4 test", t)

    # AC5 / AC6
    must("check_naked_mutate_prim_meta_3197", "AC5 build.py", build)
    must("ac3197_5_source_and_linter", "AC5 test", t)
    if "naked-mutate-prim-meta" in mut or "g_3197_" in mut or "g_3197_" in disp:
        fails.append("AC5: new public query key / g_3197_* counter")

    if (ROOT / "tests" / "issues" / "test_issue_3197.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3197.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3197.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3197.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3197-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3197 naked_mutate_prim_meta:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3197 naked_mutate_prim_meta: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
