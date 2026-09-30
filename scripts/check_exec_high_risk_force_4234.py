#!/usr/bin/env python3
# scripts/check_exec_high_risk_force_4234.py -- Issue #4234 source-cite gate.
#
# AC1: grant_capability(string) kHighRiskMask includes kEffectExec — the
#      documented prim surface forces single_use + session_bound for Exec
#      under Restricted/Strict (anti-sticky, same lifetime as Mutate).
# AC2: grant_effect_capability mask includes kEffectExec (#3561 family).
# AC3: grant_effect_durable mask includes kEffectExec — durable Exec grants
#      session-bind under production (#3177 family) and bump the durable
#      high-risk session-bound counter.
# AC4: grant_effect_durable_sticky mask includes kEffectExec — the true
#      sticky escape stays behind the #3177 gate (env + TenantAdmin + reason).
# AC5: grant_effect_session mask includes kEffectExec; runtime ACs live in
#      tests/core/test_capability_single_use_consume.cpp (ac4234_1..ac4234_5,
#      dispatched via run_test_inert_session_mid_3723); linter registered in
#      build.py + root_check_allowlist.txt; no docs/design/4234-*, no
#      tests/**/test_issue_4234.cpp.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SEC = "src/compiler/evaluator_security.cpp"
POSTURE = "src/compiler/evaluator_primitives_security.cpp"
TEST = "tests/core/test_capability_single_use_consume.cpp"
BUILD = "build.py"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_exec_high_risk_force_4234"
MASK = "kEffectExec | kEffectMutate | kEffectMacroSelfEvo | kEffectTenantAdmin |"
SITE1_MASK = "static_cast<std::uint16_t>(kEffectExec | kEffectMutate | kEffectMacroSelfEvo |"
USING = "using aura::compiler::security::kEffectExec;"
CITE = "// Issue #4234: Exec joins the production high-risk force family"

AC_FUNCS = (
    "ac4234_1_effect_path_exec_forced",
    "ac4234_2_string_path_exec_forced",
    "ac4234_3_soft_off_exec_sticky",
    "ac4234_4_durable_exec_session_bound",
    "ac4234_5_source_cite",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(sec: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def span(src: str, start: str, end: str, label: str) -> str:
        i = src.find(start)
        if i == -1:
            fails.append(f"{label}: start marker not found: {start!r}")
            return ""
        j = src.find(end, i + len(start))
        if j == -1:
            fails.append(f"{label}: end marker not found: {end!r}")
            return ""
        return src[i:j]

    # AC1 — string grant_capability (documented prim surface) forces Exec.
    s1 = span(
        sec,
        "void Evaluator::grant_capability(std::string cap) {",
        "void Evaluator::grant_capability(std::string cap, bool single_use,",
        "AC1",
    )
    if s1:
        must(SITE1_MASK, "AC1", s1)
        must(USING, "AC1", s1)
        must(CITE, "AC1", s1)

    # AC2 — grant_effect_capability (#3561 family) forces Exec.
    s2 = span(
        sec,
        "bool Evaluator::grant_effect_capability(",
        "void Evaluator::grant_effect_durable(",
        "AC2",
    )
    if s2:
        must(MASK, "AC2", s2)
        must(USING, "AC2", s2)

    # AC3 — grant_effect_durable (#3177 family) forces Exec session-bind.
    s3 = span(
        sec,
        "void Evaluator::grant_effect_durable(std::uint64_t tenant_id",
        "void Evaluator::grant_effect_durable_sticky(",
        "AC3",
    )
    if s3:
        must(MASK, "AC3", s3)
        must(USING, "AC3", s3)
        must("capability_durable_session_bound_total", "AC3", s3)

    # AC4 — grant_effect_durable_sticky keeps the #3177 gate on Exec.
    s4 = span(
        sec,
        "void Evaluator::grant_effect_durable_sticky(",
        "void Evaluator::grant_effect_session(",
        "AC4",
    )
    if s4:
        must(MASK, "AC4", s4)
        must(USING, "AC4", s4)
        must("AURA_ALLOW_DURABLE_STICKY", "AC4", s4)

    # AC5 — grant_effect_session + runtime door + registration + no invents.
    s5 = span(
        sec,
        "void Evaluator::grant_effect_session(",
        "void Evaluator::grant_cross_tenant_access(",
        "AC5",
    )
    if s5:
        must(MASK, "AC5", s5)
        must(USING, "AC5", s5)
    # Query surface (schema-2882 mask value) mirrors the grant-side SSOT.
    must(SITE1_MASK, "AC5", _read(POSTURE))
    for fn in AC_FUNCS:
        must(fn, "AC5", test)
    must(f"{LINTER}.py", "AC5", build)
    must(f"{LINTER}.py", "AC5", allow)
    if (ROOT / "tests/core/test_issue_4234.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_4234.cpp present")
    if list(ROOT.glob("docs/design/4234-*")):
        fails.append("AC5: forbidden docs/design/4234-* present")
    return fails


def _self_test() -> int:
    sec = _read(SEC)
    test = _read(TEST)
    build = _read(BUILD)
    allow = _read(ALLOW)
    bad = _rows(sec, test, build, allow)
    if bad:
        for r in bad:
            print(f"FAIL {LINTER} self-test baseline: {r}")
        return 1
    # Strip Exec from every mask; the mask gate rows must trip.
    stripped = sec.replace(MASK, "kEffectMutate | kEffectMacroSelfEvo | kEffectTenantAdmin |")
    stripped = stripped.replace(SITE1_MASK, "static_cast<std::uint16_t>(kEffectMutate | kEffectMacroSelfEvo |")
    bad2 = _rows(stripped, test, build, allow)
    if not any(r.startswith("AC") and "missing" in r for r in bad2):
        print("self-test: stripped mask did not trip the gate rows")
        return 1
    print(f"ok {LINTER} self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #4234 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any contract row")
    ap.add_argument("--self-test", action="store_true", help="run synthetic fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    rows = _rows(_read(SEC), _read(TEST), _read(BUILD), _read(ALLOW))
    if rows:
        for r in rows:
            print(f"FAIL {LINTER}: {r}")
        return 1
    print(f"ok {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
