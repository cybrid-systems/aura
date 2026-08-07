#!/usr/bin/env python3
"""Issue #2707: fail-closed mutation_id join in provenance_ok under production sandbox.

Contract:
  AC1 Production Restricted/Strict: grant bound mid=N, effect mid=0 → deny +
      provenance_mismatch / mid_join_zero_deny advance.
  AC2 Production: grant mid=N, effect mid=N+1 → deny (strict equality).
  AC3 Production: grant mid=N, effect mid=N → allow; single-use only on allow.
  AC4 Soft / Off: zero mid still skips join (legacy); no extra deny.
  AC5 Additive query keys + schema-2707; #2055/#2154/#2586 surfaces preserved.
  AC6 Source-cite + coverage linter; extend capability/security tests;
      no docs/design/* per #1655.

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


def _fn_body(hay: str, sig: str) -> str:
    i = hay.find(sig)
    if i < 0:
        return ""
    brace = hay.find("{", i)
    if brace < 0:
        return ""
    depth = 0
    for j in range(brace, len(hay)):
        if hay[j] == "{":
            depth += 1
        elif hay[j] == "}":
            depth -= 1
            if depth == 0:
                return hay[brace + 1 : j]
    return ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    cap = _read("src/core/capability_model.hh")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_require_effect_live_mid.cpp")
    build = _read("build.py")

    # AC1/AC2 — fail-closed mid join in provenance_ok.
    must("capability_mid_join_zero_deny_total", "AC1", cap)
    must("#2707", "AC1", cap)
    body = _fn_body(cap, "bool provenance_ok")
    if not body:
        body = _fn_body(cap, "provenance_ok(TenantId")
    if not body:
        fails.append("AC1: provenance_ok impl not found")
    else:
        must("fail_closed_mid", "AC1", body)
        must("EffectSandboxMode::Restricted", "AC1", body)
        must("EffectSandboxMode::Strict", "AC1", body)
        must("capability_mid_join_zero_deny_total", "AC1", body)
        # Zero prov mid under production → deny.
        if "prov.mutation_id == 0" not in body and "prov.mutation_id != 0" not in body:
            fails.append("AC1: provenance_ok must special-case zero prov.mutation_id")
        # Production branch must not only use the soft skip-when-zero form.
        soft_skip = (
            "g.bound_mutation_id != 0 && prov.mutation_id != 0 &&" in body.replace("\n", " ")
            or "g.bound_mutation_id != 0 && prov.mutation_id != 0" in body
        )
        hard_eq = "g.bound_mutation_id != prov.mutation_id" in body
        if not hard_eq:
            fails.append("AC2: provenance_ok must strict-compare mid under fail-closed")
        if not soft_skip:
            fails.append("AC4: Soft/Off skip-when-zero branch must remain")

    # AC3 — single-use only on allow (existing path, still present).
    must("single_use", "AC3", cap)
    must("#2586", "AC3", cap)
    must("capability_single_use_consumed", "AC3", cap)

    # AC4 — Soft path comment / branch.
    must("Soft / Off", "AC4", cap)
    must("skip-when-zero", "AC4", cap)

    # AC5 — query surface.
    must("mid-join-zero-deny", "AC5", q)
    must("mid-join-fail-closed-armed", "AC5", q)
    must("schema-2707", "AC5", q)
    must("issue-2707", "AC5", q)
    must("provenance-mismatch", "AC5", q)  # existing preserved
    must("#2055", "AC5", cap)
    must("#2154", "AC5", cap)

    # AC6 — tests + linter + no design docs.
    must("#2707", "AC6", test)
    must("mid_join_zero_deny", "AC6", test + cap)
    must("check_mid_join_fail_closed_2707", "AC6", build)
    for rel in (
        "docs/design/mid_join_fail_closed_2707.md",
        "docs/mid_join_fail_closed_2707.md",
        "design/2707.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2707 fail-closed mutation_id mid join under production sandbox — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
