#!/usr/bin/env python3
"""Issue #3845: promote_sampled_force_join_mid must not invent mid.

Residual: #3367 hardened pin_composite_batch_join_mid so mid==0 does not
invent process-origin mid. promote_sampled_force_join_mid still called
next_audit_mutation_id() when mid==0 — same hole for Sampled+force.

Contract (one row per AC):
  AC1  Hard face (production_defaults || Full): promote(0) → 0; no sticky;
       no next_audit_mutation_id invent
  AC2  Non-zero deny_mid path unchanged
  AC3  Soft invent intentionally NOT kept (documented; aligned pin #3367)
  AC4  Extends test_audit_mutation_id_unify; linter + grandfather +
       manifest + build.py; not a dup of #3837/#3838; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

TMA = "src/compiler/typed_mutation_audit.h"
TEST = "tests/compiler/test_audit_mutation_id_unify.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3845.json"
LINTER = "check_promote_force_join_mid_no_invent_3845"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    tma = _read(TMA)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    # Locate promote function window
    promo = tma.find("inline std::uint64_t promote_sampled_force_join_mid")
    if promo < 0:
        fails.append("AC1: promote_sampled_force_join_mid not found")
        promo_win = ""
    else:
        promo_win = tma[promo : promo + 1200]

    # AC1 — hard face no invent
    must("Issue #3845", "AC1 cite tma", tma)
    must("production_defaults_active()", "AC1 hard gate in promote", promo_win)
    must("AuditStrategy::Full", "AC1 Full in promote hard gate", promo_win)
    must_not("next_audit_mutation_id()", "AC1 no invent in promote", promo_win)
    must("return 0", "AC1 mid==0 returns 0", promo_win)

    # AC2 — non-zero path still sticky-pins
    must("g_tls_composite_batch_join_mid = mid", "AC2 sticky assign", promo_win)
    must("note_boundary_audit_mid(mid)", "AC2 note boundary", promo_win)

    # AC3 — Soft invent NOT kept, documented
    must("Soft invent", "AC3 Soft invent doc", tma)
    must("intentionally NOT kept", "AC3 Soft invent not kept", tma)
    must("Soft quiet — no invent", "AC3 Soft quiet return", promo_win)

    # AC4 — suite + wiring; not dup of #3837/#3838
    must("ac3845_1_hard_promote_zero_no_invent", "AC4 test AC1", test)
    must("ac3845_2_nonzero_deny_mid_unchanged", "AC4 test AC2", test)
    must("ac3845_3_soft_no_observe_invent_documented", "AC4 test AC3", test)
    must("ac3845_4_source_cite_wiring_no_invent", "AC4 test AC4", test)
    must("Issue #3845", "AC4 test cite", test)
    must("#3837", "AC4 not-dup-of-3837 cite", test + tma)
    must("#3838", "AC4 not-dup-of-3838 cite", test + tma)
    must("#3367", "AC4 cites pin #3367", tma)
    must(LINTER, "AC4 build registration", build)
    must("Issue #3845", "AC4 build cite", build)
    must(f"{LINTER}.py", "AC4 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC4 grandfather path", gf)
    must('"issue": 3845', "AC4 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC4: {MANIFEST} missing")
    for rel in (
        "tests/compiler/test_issue_3845.cpp",
        "tests/core/test_issue_3845.cpp",
        "tests/issues/test_issue_3845.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3845*"):
            fails.append(f"AC4: docs/design/{p.name} exists")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qr = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    must_not("schema-3845", "AC4 no new schema key", q + qr)
    must_not("query:promote-force-join-mid", "AC4 no new query key", q + qr)

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3845", file=sys.stderr)
        return 1

    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
