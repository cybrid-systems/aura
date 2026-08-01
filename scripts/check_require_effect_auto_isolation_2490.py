#!/usr/bin/env python3
"""Issue #2490: require_effect auto-enforces workspace isolation (single
side-effect entry). Pure / zero-bits callers unchanged; isolation deny
short-circuits before effect check; single SE IsolationDeny count (#2388).

Contract:
  AC1 Restricted + unset principal → IsolationDeny (no side effect runs)
  AC2 Restricted + principal + grant → allow (capability + isolation both pass)
  AC3 Cross-tenant / repeat-call → exactly one IsolationDeny SE per call
  AC4 Off sandbox + tenant=0 → permissive
  AC5 Existing prims that only call require_effect gain isolation
  AC6 Source-cite + tests + CMake + build.py gate + this linter present

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

    sec = _read("src/compiler/evaluator_security.cpp")
    test = _read("tests/compiler/test_require_effect_auto_isolation_2490.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # Locate require_effect body.
    req_i = sec.find("bool Evaluator::require_effect")
    if req_i < 0:
        fails.append("AC1: require_effect definition missing")
        body = ""
    else:
        # Generous window — the function is short but the comment block
        # ahead explains the new auto-isolation contract.
        body = sec[req_i : req_i + 2500]

    # AC1/AC2: auto-isolation wired before effect check.
    must("Issue #2490", "AC1", sec)
    must("check_workspace_isolation", "AC1", body)
    must("req_bits != 0", "AC1", body)
    # Isolation check must precede effect check (single entry).
    iso_pos = body.find("check_workspace_isolation")
    eff_pos = body.find("check_and_record_effect")
    if iso_pos < 0 or eff_pos < 0:
        fails.append("AC1: missing isolation / effect call in require_effect")
    elif iso_pos > eff_pos:
        fails.append("AC1: isolation check must precede effect check")
    # Pure / zero-bits callers unchanged — must NOT call isolation when
    # req_bits == 0 (gated by the if).
    must("if (req_bits != 0)", "AC1", body)

    # AC2: existing principal + grant allow path still present (and #2384
    # live-mid provenance wiring is preserved — referenced in the include
    # comment at line 14 plus the AC6 cite).
    must("check_and_record_effect", "AC2", body)
    must("#2384", "AC2", sec)

    # AC3: single-count contract preserved — comment cites #2388.
    must("#2388", "AC3", body)

    # AC4: Off sandbox path unchanged — capability check still allows.
    # (No explicit gate needed; the if (req_bits != 0) guards both.)

    # AC5: existing prims gain isolation — confirmed by AC1 test (same
    # wiring). Linter only needs to confirm the gate is present.
    must("req_bits != 0", "AC5", body)

    # AC6 — registrations + test ac functions + this linter self-cite.
    must("ac1_restricted_unset_principal_denies", "AC1", test)
    must("ac2_restricted_principal_grant_allows", "AC2", test)
    must("ac3_single_isolation_deny_count", "AC3", test)
    must("ac4_off_sandbox_permissive", "AC4", test)
    must("ac5_existing_prims_gain_isolation", "AC5", test)
    must("ac6_source_and_gate", "AC6", test)
    must("Issue #2490", "AC6", test)
    must("Issue #2490", "AC6", sec)
    must("test_require_effect_auto_isolation_2490", "AC6", cmake)
    must("aura_add_issue_test(test_require_effect_auto_isolation_2490)", "AC6", cmake)
    must("aura_issue_test_link_llvm_jit(test_require_effect_auto_isolation_2490)", "AC6", cmake)
    must("check_require_effect_auto_isolation_2490", "AC6", build)
    must("cmd_require_effect_auto_isolation_2490_coverage", "AC6", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2490 require_effect auto-isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
