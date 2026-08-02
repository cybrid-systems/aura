#!/usr/bin/env python3
"""Issue #2494: hard-fail check_side_effect_security.py for new prims without
require_effect / PrimMeta. The script already had --strict mode + allowlist
reason enforcement; this gate enforces PR CI integration + script self-test
mechanism (`--path` argument) so a fixture prim proves the gate catches
missing coverage.

Contract:
  AC1 Intentionally broken fixture prim → script exits non-zero under --strict
  AC2 Existing production prim set passes under --strict (no false-positive)
  AC3 PR CI gate runs the script via build.py gate (which uses --strict)
  AC4 Allowlist entries missing '# SECURITY_EXEMPT: <reason>' → fail
  AC5 Source-cite script + CI workflow path + #2057 rule doc
  AC6 Tests + source-cite registrations (CMakeLists.txt + build.py + linter)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    script = _read("scripts/check_side_effect_security.py")
    bp = _read("build.py")
    ci = _read(".github/workflows/ci.yml")
    header = _read("src/compiler/security_side_effect.hh")
    allow = _read("tests/side-effect-security-allowlist.txt")
    test = _read("tests/compiler/test_side_effect_security_gate_hardfail_2494.cpp")
    cmake = _read("CMakeLists.txt")

    # AC1 / AC2 — --strict + --path + scan() signature.
    must("Issue #2494", "AC1", script)
    must('"--strict"', "AC1", script)
    must('"--path"', "AC1", script)
    must("scan(path_override", "AC1", script)
    must("EXEMPT_REASON_RE", "AC4", script)

    # AC3 — CI gate wiring.
    must("def cmd_side_effect_security():", "AC3", bp)
    must("--strict", "AC3", bp)
    must("or cmd_side_effect_security()", "AC3", bp)
    must("build.py gate", "AC3", ci)

    # AC4 — allowlist reason format.
    must("SECURITY_EXEMPT:", "AC4", allow)

    # AC5 — source-cite script + CI workflow + rule doc.
    must("Issue #2057", "AC5", header)

    # AC6 — registrations + test ac functions.
    must("ac1_broken_fixture_fails", "AC1", test)
    must("ac2_existing_prim_set_passes", "AC2", test)
    must("ac3_ci_gate_runs_script", "AC3", test)
    must("ac4_allowlist_reason_enforced", "AC4", test)
    must("ac5_source_cite", "AC5", test)
    must("ac6_registrations", "AC6", test)
    must("Issue #2494", "AC6", test)
    must("test_side_effect_security_gate_hardfail_2494", "AC6", cmake)
    must(
        "aura_add_issue_test(test_side_effect_security_gate_hardfail_2494)",
        "AC6",
        cmake,
    )
    must(
        "aura_issue_test_link_light(test_side_effect_security_gate_hardfail_2494)",
        "AC6",
        cmake,
    )
    must("check_side_effect_security_gate_hardfail_2494", "AC6", bp)
    must("cmd_side_effect_security_gate_hardfail_2494_coverage", "AC6", bp)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2494 side-effect security gate hard-fail — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
