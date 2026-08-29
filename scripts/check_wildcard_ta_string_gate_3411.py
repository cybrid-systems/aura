#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #3411: has_capability("*") string-gate must not short-circuit
# TA/MSE bits. Close the double-track with the #3144 effects_for strip.
# set_tenant_principal(allow_cross=true) privileged check drops the
# standalone has_capability(kCapWildcard) arm.
#
# AC1 — Evaluator::has_capability computes eff BEFORE the wildcard
#       short-circuit; is_ta_mse_eff guard routes TA/MSE queries through
#       effects_for (#3144 strip). Wildcard-only cannot satisfy TA/MSE.
# AC2 — set_tenant_principal privileged check drops the
#       has_capability(kCapWildcard) arm. SE reason
#       'allow-cross-needs-tenant-admin' preserved.
# AC3 — Wildcard still grants non-TA/MSE effect-mapped caps + string-only
#       caps. Existing string-only caps list path preserved.
# AC4 — Soft/Off short-circuit at top of has_capability (zero extra).
# AC5 — #3141 string write fence, #3144 effects_for strip, #3363
#       require_effect TA deny, #3010 write gate, #3332 isolation read
#       path all preserved (no regression).
# AC6 — No docs/design/3411-*.md (banned per #1655) and no
#        tests/{issues,compiler,core}/test_issue_3411.cpp (must extend
#        test_tenant_isolation_enforcement.cpp per #81934).
# AC7 — test_tenant_isolation_enforcement.cpp carries AC1-AC6 markers
#        for #3411; build.py wires cmd_wildcard_ta_string_gate_3411.
#
# Self-test:
#   python3 scripts/check_wildcard_ta_string_gate_3411.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring
    search does not false-positive on prose. Cheap state machine; good
    enough for source-cite checks (does not need to handle raw strings /
    trigraphs).
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def main() -> int:
    fails: list[str] = []

    sec = (ROOT / "src" / "compiler" / "evaluator_security.cpp").read_text()
    cap = (ROOT / "src" / "core" / "capability_model.hh").read_text()
    wiso = (ROOT / "src" / "core" / "workspace_isolation.hh").read_text()
    test = (ROOT / "tests" / "core" / "test_tenant_isolation_enforcement.cpp").read_text()
    build = (ROOT / "build.py").read_text()

    # AC1 — has_capability computes eff before wildcard short-circuit + TA/MSE routing.
    if "Issue #3411" not in sec:
        fails.append("AC1: evaluator_security.cpp missing 'Issue #3411' marker")
    eff_pos = sec.find("const Effect eff = effect_for_cap_name(needed);")
    wild_pos = sec.find("if (wildcard_held)")
    if eff_pos < 0:
        fails.append("AC1: evaluator_security.cpp missing 'const Effect eff = effect_for_cap_name(needed);' line")
    if wild_pos < 0:
        fails.append("AC1: evaluator_security.cpp missing 'if (wildcard_held)' short-circuit")
    if eff_pos > 0 and wild_pos > 0 and eff_pos > wild_pos:
        fails.append(
            "AC1: eff must be computed BEFORE the wildcard short-circuit "
            "(otherwise is_ta_mse_eff guard cannot steer TA/MSE to effects_for)"
        )
    if "is_ta_mse_eff" not in sec:
        fails.append("AC1: evaluator_security.cpp missing is_ta_mse_eff guard for TA/MSE routing")
    if "Effect::TenantAdmin" not in sec or "Effect::MacroSelfEvo" not in sec:
        fails.append("AC1: TA / MSE bits not referenced in has_capability TA/MSE guard")
    if "has_effect(g_capability_registry().effects_for(capability_tenant_id_), eff)" not in sec:
        fails.append("AC1: TA/MSE routed via effects_for path missing")

    # AC2 — set_tenant_principal drops kCapWildcard privileged arm.
    set_tp_pos = sec.find("set_tenant_principal(std::uint64_t tenant_id")
    if set_tp_pos < 0:
        fails.append("AC2: evaluator_security.cpp missing set_tenant_principal definition")
    else:
        # 2400-char window captures the full body (privileged check + SE emit
        # reason line). Comments stripped so a 'kCapWildcard' mention in the
        # explanatory comment does NOT false-positive the code-search.
        priv_block = _strip_cpp_comments(sec[set_tp_pos : set_tp_pos + 2400])
        if "has_capability(kCapTenantAdmin)" not in priv_block:
            fails.append("AC2: privileged OR missing has_capability(kCapTenantAdmin)")
        if "has_capability(kCapCapability)" not in priv_block:
            fails.append("AC2: privileged OR missing has_capability(kCapCapability)")
        if "has_capability(kCapWildcard)" in priv_block:
            fails.append("AC2: privileged OR still arms has_capability(kCapWildcard) — wildcard持卡不算 TA per #3144")
        if "allow-cross-needs-tenant-admin" not in priv_block:
            fails.append("AC2: SE reason 'allow-cross-needs-tenant-admin' missing")
        if "force_bind = sandbox_mode_" not in priv_block:
            fails.append("AC2: Soft/Off short-circuit via force_bind missing")

    # AC3 — Wildcard still grants non-TA/MSE + string-only caps.
    if "wildcard_held" not in sec or "if (wildcard_held)" not in sec:
        fails.append("AC3: wildcard short-circuit must remain for non-TA/MSE + string-only")
    if "Legacy string-only caps keep the list path." not in sec:
        fails.append("AC3: string-only caps list path comment / code missing")

    # AC4 — Soft/Off short-circuit at top.
    if "Sandbox fully off" not in sec:
        fails.append("AC4: top-of-has_capability Soft/Off short-circuit comment missing")
    if "!sandbox_mode_ && effect_sandbox_mode() == 0" not in sec:
        fails.append("AC4: top-of-has_capability Soft/Off predicate missing")

    # AC5 — Existing fences not regressed.
    if "Issue #3141" not in sec:
        fails.append("AC5: #3141 string write fence regressed (missing in evaluator_security.cpp)")
    if "Issue #3144" not in cap:
        fails.append("AC5: #3144 effects_for strip regressed (missing in capability_model.hh)")
    # #3363 require_effect TA deny is anchored in capability_model.hh
    # (effects_for strip is the same path #3144 + #3363 harden).
    if "Issue #3363" not in cap:
        fails.append("AC5: #3363 require_effect TA deny regressed (missing in capability_model.hh)")
    if "Issue #3010" not in sec:
        fails.append("AC5: #3010 write gate regressed (missing in evaluator_security.cpp)")
    # #3332 isolation read path is anchored in workspace_isolation.hh
    # (set_tenant_principal README-style comment + the read-path fence).
    if "Issue #3332" not in wiso:
        fails.append("AC5: #3332 isolation read path regressed (missing in workspace_isolation.hh)")

    # AC6 — No design docs / no test_issue_3411.cpp.
    if list((ROOT / "docs" / "design").glob("3411-*.md")):
        fails.append("AC6: docs/design/3411-*.md exists — design docs banned per #1655")
    for sub in ("issues", "compiler", "core"):
        if (ROOT / "tests" / sub / "test_issue_3411.cpp").is_file():
            fails.append(
                f"AC6: tests/{sub}/test_issue_3411.cpp exists — must extend existing "
                "test_tenant_isolation_enforcement.cpp per #81934"
            )

    # AC7 — Test markers + build.py wiring.
    if "3411 AC1: #3411 marker" not in test:
        fails.append("AC7: test_tenant_isolation_enforcement.cpp missing 3411 AC1 marker")
    if "3411 AC2: set_tenant_principal drops kCapWildcard privileged arm" not in test:
        fails.append("AC7: test_tenant_isolation_enforcement.cpp missing 3411 AC2 marker")
    if "3411 AC3: wildcard still grants non-TA/MSE + string-only" not in test:
        fails.append("AC7: test_tenant_isolation_enforcement.cpp missing 3411 AC3 marker")
    if "3411 AC4: Soft/Off zero-cost short-circuit" not in test:
        fails.append("AC7: test_tenant_isolation_enforcement.cpp missing 3411 AC4 marker")
    if "3411 AC5: #3141/#3363/#3332 don't regress" not in test:
        fails.append("AC7: test_tenant_isolation_enforcement.cpp missing 3411 AC5 marker")
    if "3411 AC6: no docs/design/3411-*; no test_issue_3411.cpp" not in test:
        fails.append("AC7: test_tenant_isolation_enforcement.cpp missing 3411 AC6 marker")

    if "cmd_wildcard_ta_string_gate_3411_coverage" not in build:
        fails.append("AC7: build.py does not register cmd_wildcard_ta_string_gate_3411_coverage")
    if "check_wildcard_ta_string_gate_3411" not in build:
        fails.append("AC7: build.py does not register check_wildcard_ta_string_gate_3411 linter script")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1

    print("PASS: #3411 has_capability wildcard TA/MSE string-gate contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
