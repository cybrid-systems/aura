#!/usr/bin/env python3
"""Issue #3843: require_effect hard mid refuse matches Typed resolve.

Residual: resolve_audit_mutation_id / production_deny_se_mid / emit_mutation_audit
treat production_defaults_active() || AuditStrategy::Full as hard mid refuse
(mid=0 + mid-fallback-refused). Evaluator::require_effect only gated on
production_defaults_active() and invented mid=1 otherwise — Full-without-
defaults (cold-start Full + production_defaults==0) stamped SE/grant with
phantom mid=1 while Typed trail refused mid=0.

Contract (one row per AC):
  AC1  require_effect hard = production_defaults || Full; mid==0 refuses
       (join_audit_and_se_mid then return false); no phantom mid=1 invent
  AC2  Soft-only mid=1 observe stamp retained (else-if mid==0 → mid=1)
  AC3  Extends test_audit_mutation_id_unify; linter + grandfather +
       manifest + build.py; not a dup of #3837; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SEC = "src/compiler/evaluator_security.cpp"
TEST = "tests/compiler/test_audit_mutation_id_unify.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3843.json"
LINTER = "check_require_effect_full_hard_mid_3843"


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

    sec = _read(SEC)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    re = sec.find("bool Evaluator::require_effect")
    if re < 0:
        fails.append("AC1: Evaluator::require_effect not found")
        re_win = ""
    else:
        re_win = sec[re : re + 4000]

    # AC1 — hard face includes Full; refuse on join mid==0.
    must("Issue #3843", "AC1 cite", sec)
    must("AuditStrategy::Full", "AC1 Full in require_effect", re_win)
    must("production_defaults_active()", "AC1 production_defaults in hard", re_win)
    must("join_audit_and_se_mid(mid)", "AC1 join on hard path", re_win)
    must("if (mid == 0)", "AC1 mid==0 refuse", re_win)
    must("return false", "AC1 refuse return", re_win)
    if "AuditStrategy::Full" not in re_win and "production_hard_face_active()" not in re_win:
        fails.append("AC1: require_effect hard face missing Full / production_hard_face_active")
    soft_idx = re_win.find("mid = 1")
    if soft_idx < 0:
        fails.append("AC2: Soft mid=1 invent arm missing")
    elif "else if (mid == 0)" not in re_win[max(0, soft_idx - 80) : soft_idx + 40]:
        fails.append("AC2: mid=1 invent must be under else if (mid == 0)")

    # AC2 Soft comment / Soft-only wording.
    must("Soft only", "AC2 Soft-only comment", re_win)

    # AC3 — suite + wiring + no invent; not a dup of #3837.
    must("ac3843_1_full_without_defaults_refuses", "AC3 test AC1", test)
    must("ac3843_2_soft_mid1_unchanged", "AC3 test AC2", test)
    must("ac3843_3_source_cite_wiring_no_invent", "AC3 test AC3", test)
    must("Issue #3843", "AC3 test cite", test)
    must("#3837", "AC3 not-dup-of-3837 cite", test + sec)
    must(LINTER, "AC3 build registration", build)
    must("Issue #3843", "AC3 build cite", build)
    must(f"{LINTER}.py", "AC3 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC3 grandfather path", gf)
    must('"issue": 3843', "AC3 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC3: {MANIFEST} missing")
    for rel in (
        "tests/compiler/test_issue_3843.cpp",
        "tests/core/test_issue_3843.cpp",
        "tests/issues/test_issue_3843.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC3: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3843*"):
            fails.append(f"AC3: docs/design/{p.name} exists")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qr = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    must_not("schema-3843", "AC3 no new schema key", q + qr)
    must_not("query:require-effect-full-hard", "AC3 no new query key", q + qr)

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3843", file=sys.stderr)
        return 1

    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
