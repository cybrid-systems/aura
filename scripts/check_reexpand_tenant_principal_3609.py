#!/usr/bin/env python3
# scripts/check_reexpand_tenant_principal_3609.py -- Issue #3609 source-cite gate.
#
# AC1: the post_mutation_macro_reexpand MacroSelfEvo choke consumes the
#      live Evaluator principal (tenant_for_macro_self_evo_check) — the
#      process default_tenant read is gone from evaluator_eval_flat.cpp —
#      and the deny stamps the #3304 capability-deny sentinel.
# AC2: the #3378 helper is exported (non-static definition + ixx export
#      declaration) instead of a duplicated third tenant resolver.
# AC3: existing counters + #3028 reason surface preserved; #3594 live
#      join mid args preserved.
# AC4: Soft/Off contract unchanged — is_sandbox_active still gates the
#      check (check_macro_self_evo is unconstrained under Soft/Off).
# AC5: wildcard_ok stays false on the choke.
# AC6: both self-evo suites extended (chokepoint + evaluator principal);
#      build.py wires this linter; no docs/design/*3609*, no
#      tests/**/test_issue_3609.cpp.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

EFL = "src/compiler/evaluator_eval_flat.cpp"
MX = "src/compiler/macro_expansion.cpp"
MIXX = "src/compiler/macro_expansion.ixx"
CHOKE_TEST = "tests/compiler/test_macro_self_evo_reexpand_chokepoint.cpp"
PRIN_TEST = "tests/compiler/test_macro_self_evo_check_evaluator_principal.cpp"
BUILD = "build.py"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    _ = ap.parse_args()

    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    efl = _read(EFL)
    mx = _read(MX)
    mixx = _read(MIXX)
    choke_test = _read(CHOKE_TEST)
    prin_test = _read(PRIN_TEST)
    build = _read(BUILD)

    # AC1
    must("Issue #3609", "AC1 evaluator_eval_flat", efl)
    must("macro_exp::tenant_for_macro_self_evo_check()", "AC1 choke principal", efl)
    must_not("g_capability_registry().default_tenant.load()", "AC1 no default_tenant", efl)
    must("macro_exp::kHygieneLimitReasonCapabilityDeny", "AC1 deny sentinel", efl)
    must("Issue #3609", "AC1 choke test cites", choke_test)
    must("3609: choke resolves the live Evaluator principal", "AC1 test row", choke_test)

    # AC2
    must("export std::uint16_t tenant_for_macro_self_evo_check() noexcept", "AC2 ixx export", mixx)
    must("[[nodiscard]] std::uint16_t tenant_for_macro_self_evo_check() noexcept", "AC2 non-static def", mx)
    must_not("[[nodiscard]] static std::uint16_t tenant_for_macro_self_evo_check()", "AC2 no static def", mx)

    # AC3
    must("g_macro_self_evo_denied_total.fetch_add(1", "AC3 deny counter", efl)
    must("g_macro_clone_last_reject_reason.store(1", "AC3 reason surface", efl)
    must("join_audit_and_se_mid(0)", "AC3 #3594 join mid", efl)

    # AC4
    must("is_sandbox_active", "AC4 sandbox gate", efl)

    # AC5
    must("wildcard_ok=*/false", "AC5 wildcard false", efl)

    # AC6
    must("check_reexpand_tenant_principal_3609", "AC6 build wiring", build)
    must("3609: post_mutation_macro_reexpand uses the live principal", "AC6 principal test row", prin_test)
    must("3609: choke resolves the live Evaluator principal", "AC6 choke test row", choke_test)
    if (ROOT / "tests" / "compiler" / "test_issue_3609.cpp").is_file():
        fails.append("AC6: test_issue_3609.cpp present (forbidden per #81934/#81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3609*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3609 reexpand tenant principal — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
