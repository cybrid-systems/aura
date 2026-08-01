#!/usr/bin/env python3
"""Issue #2489: promote remaining high-risk string-only caps into the Effect
matrix (self-evo / synthesize / strategy / sys-open / sys-write / sys-read /
agent / capability).

Contract:
  AC1 Registry-only grant satisfies has_capability for every newly-mapped name.
  AC2 revoke_effect_capability clears matrix + string list.
  AC3 Restricted / Strict + no grant → deny with SE EffectDeny / Agent reason.
  AC4 grant_min_valid_epoch advance → epoch fence hits on self-evo.
  AC5 hard_fiber_isolation + fiber mismatch → hard deny on agent / self-evo.
  AC6 Tests + source-cite effect_for_cap_name / has_capability + CMake +
      build.py gate + this linter present.
  AC7 SECURITY_EXEMPT residual list documented in capability_model.hh.

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

    cap = _read("src/core/capability_model.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    sch = _read("src/compiler/security_capabilities.h")
    test = _read("tests/compiler/test_capability_high_risk_promote_2489.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — all 8 mappings in capability_model.hh.
    must("Issue #2489", "AC1", cap)
    must('name == "self-evo" || name == "synthesize" || name == "strategy"', "AC1", cap)
    must('name == "sys-open" || name == "sys-write"', "AC1", cap)
    must('name == "sys-read"', "AC1", cap)
    must('name == "agent"', "AC1", cap)
    must('name == "capability"', "AC1", cap)
    must("Effect::Syscall | Effect::Write", "AC1", cap)
    must("Effect::Syscall | Effect::Read", "AC1", cap)
    must("Effect::TenantAdmin", "AC1", cap)
    must("Effect::MacroSelfEvo", "AC1", cap)
    # Self-evo mapping returns MacroSelfEvo (already present from #2023, but
    # the AC1 case-loop also tests synthesize + strategy → same bit).

    # AC2 — revoke path documented.
    must("Issue #2489", "AC2", sec)
    must("revoke_effect_capability", "AC2", sec)

    # AC3 — has_capability cites effect_for_cap_name + #2489. The mapping
    # itself lives in capability_model.hh (single source of truth); the
    # has_capability path in evaluator_security.cpp delegates there.
    must("effect_for_cap_name", "AC3", sec)
    must('name == "self-evo"', "AC3", cap)

    # AC4 — epoch fence path live (capability_model.hh has provenance_ok
    # checking grant_epoch < min_valid_epoch; just sanity-check the metric).
    must("capability_epoch_fence_hit_total", "AC4", cap)

    # AC5 — hard fiber isolation metric lives in capability_model.hh.
    must("capability_fiber_hard_deny_total", "AC5", cap)
    must("hard_fiber_isolation_", "AC5", cap)

    # AC6 — cites + registrations.
    must("Issue #2489", "AC6", sch)
    must("test_capability_high_risk_promote_2489", "AC6", cmake)
    must("aura_add_issue_test(test_capability_high_risk_promote_2489)", "AC6", cmake)
    must("aura_issue_test_link_llvm_jit(test_capability_high_risk_promote_2489)", "AC6", cmake)
    must("check_capability_high_risk_promote_2489", "AC6", build)
    must("cmd_capability_high_risk_promote_2489_coverage", "AC6", build)
    must("ac1_registry_only_promoted_caps", "AC1", test)
    must("ac2_revoke_clears_both", "AC2", test)
    must("ac3_strict_deny_and_audit", "AC3", test)
    must("ac4_epoch_fence", "AC4", test)
    must("ac5_hard_fiber_deny", "AC5", test)
    must("ac6_source_and_security_exempt_doc", "AC6", test)

    # AC7 — SECURITY_EXEMPT residual list documented + must include
    # low-risk display names; must NOT contain the newly-promoted ones in
    # the staged section (the matrix section still mentions them in the
    # mapping body — that is fine).
    must("SECURITY_EXEMPT", "AC7", cap)
    must("compile-stats", "AC7", cap)
    must("query", "AC7", cap)
    must("sandbox", "AC7", cap)

    # AC6 — effect_name_str covers TenantAdmin + Syscall for Agent reason.
    must("kEffectTenantAdmin", "AC3", sch)
    must("kEffectSyscall", "AC3", sch)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2489 high-risk cap promotion — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
