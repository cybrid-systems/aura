#!/usr/bin/env python3
"""Issue #3836: shell/command-output require_effect(Exec) (git-commit sibling).

String-cap deny_exec alone skipped fiber-principal / isolation / live mid
under Restricted. shell + command-output now call require_effect(kEffectExec)
before fork/execl/popen. Soft/Off deny_exec (!sandbox_mode()) short-circuit
is contract — effect choke only when sandbox on / production face.

Contract (one row per AC):
  AC1  shell + command-output call require_effect(kEffectExec) before fork/popen
  AC2  Soft/Off deny_exec !sandbox_mode() short-circuit retained
  AC3  EXEMPT_2ARG + counts + build/grandfather wiring; no invent / docs

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


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    filep = _read("src/compiler/evaluator_primitives_file.cpp")
    io_cpp = _read("src/compiler/evaluator_primitives_io.cpp")
    test = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    test2 = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    gf = _read("scripts/coverage/simple_check_grandfather.txt")
    fiber = _read("scripts/coverage/checks/check_side_effect_fiber_principal_2839.py")
    mandate = _read("scripts/coverage/checks/check_side_effect_node_id_mandate_2942.py")
    ixx = _read("src/compiler/evaluator.ixx")

    # ── AC1 ──
    must("Issue #3836", "AC1 cite shell", filep)
    must('require_effect(kEffectExec, "shell")', "AC1 shell", filep)
    must('require_effect(kEffectExec, "command-output")', "AC1 command-output", filep)
    must("3836 AC1", "AC1 test auto_isolation", test)
    # order: require_effect before fork / popen
    sp = filep.find('require_effect(kEffectExec, "shell")')
    fp = filep.find("::fork()", sp if sp >= 0 else 0)
    if sp < 0 or fp < 0 or fp < sp:
        fails.append("AC1: shell require_effect not before fork")
    cp = filep.find('require_effect(kEffectExec, "command-output")')
    pp = filep.find("::popen(", cp if cp >= 0 else 0)
    if cp < 0 or pp < 0 or pp < cp:
        fails.append("AC1: command-output require_effect not before popen")
    # git-commit sibling retained
    must("kEffectExec", "AC1 git-commit Exec retained", io_cpp)
    must('"git-commit"', "AC1 git-commit op retained", io_cpp)

    # ── AC2 Soft/Off ──
    must("3836 AC3", "AC2/AC3 Soft test cite", test)
    must("!ev.sandbox_mode()", "AC2 deny_exec Soft short-circuit", filep)
    must("deny_exec", "AC2 deny_exec retained", filep)
    # Soft contract: deny_exec still short-circuits when !sandbox_mode
    if not re.search(
        r"const auto deny_exec = \[&ev\][\s\S]{0,400}?!ev\.sandbox_mode\(\)",
        filep,
    ):
        fails.append("AC2: deny_exec missing !sandbox_mode() short-circuit")

    # ── AC3 EXEMPT + wiring + forbid invent ──
    must('"shell"', "AC3 fiber EXEMPT shell", fiber)
    must('"command-output"', "AC3 fiber EXEMPT command-output", fiber)
    must('"shell"', "AC3 mandate EXEMPT shell", mandate)
    must('"command-output"', "AC3 mandate EXEMPT command-output", mandate)
    must("kResidualNodeIdExemptOpsCount = 7", "AC3 exempt count", ixx)
    must("kNodeIdMandateExemptOpsCount = 7", "AC3 mandate count", ixx)
    must("kResidualNodeIdInventoryCount = 24", "AC3 inventory count", ixx)
    must("check_shell_require_effect_3836", "AC3 build.py", build)
    must("Issue #3836", "AC3 build cite", build)
    must("check_shell_require_effect_3836.py", "AC3 grandfather basename", gf)
    must(
        "scripts/coverage/checks/check_shell_require_effect_3836.py",
        "AC3 grandfather path",
        gf,
    )
    must("3836 AC3", "AC3 tenant test", test2)
    must_not("schema-3836", "AC3 no schema key", _read("src/compiler/evaluator_primitives_security.cpp"))
    must_not("issue-3836", "AC3 no issue key", _read("src/compiler/evaluator_primitives_security.cpp"))
    for rel in (
        "tests/core/test_issue_3836.cpp",
        "tests/compiler/test_issue_3836.cpp",
        "tests/issues/test_issue_3836.cpp",
    ):
        if _read(rel):
            fails.append(f"AC3: {rel} exists — forbidden")
    docs_design_dir = ROOT / "docs" / "design"
    if docs_design_dir.is_dir():
        for f in docs_design_dir.glob("3836-*.md"):
            fails.append(f"AC3: {f.relative_to(ROOT)} exists — forbidden per #1655")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK #3836 shell_require_effect: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
