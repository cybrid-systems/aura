#!/usr/bin/env python3
"""Issue #2948: SSOT resolve_bp_threshold for spawn admit + watch degrade.

Contract (one row per AC):
  AC1  nullopt spawn → same as resolve_mailbox_bp_admit_threshold
  AC2  policy 0 → process default; watch uses resolve_bp_threshold
  AC3  spec 0 → always_reject (spec-admit-off); documented asymmetry
  AC4  load_mailbox_bp_recent shared by spawn + watch
  AC5  additive schema-2948 + resolve counter; Soft unchanged
  AC6  tests + build.py; no invent/design

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


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    spawn = _read("src/orch/agent_spawn.h")
    scope = _read("src/orch/agent_scope.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    test_admit = _read("tests/orch/test_per_scope_bp_admit.cpp")
    test_pol = _read("tests/orch/test_agent_failure_policy.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2948", "AC1", spawn)
    must("resolve_bp_threshold", "AC1", spawn)
    must("BpThresholdDecision", "AC1", spawn)
    must("resolve_mailbox_bp_admit_threshold", "AC1", spawn)
    must("2948 AC1", "AC1", test_admit)

    # AC2
    must("policy_zero_means_process_default", "AC2", spawn)
    must("resolve_bp_threshold", "AC2", scope)
    must("2948 AC2", "AC2", test_pol)

    # AC3
    must("spec-admit-off", "AC3", spawn)
    must("always_reject", "AC3", spawn)
    must("ALWAYS REJECT", "AC3", spawn)
    must("2948 AC3", "AC3", test_admit)
    must("resolve_bp_threshold", "AC3", readme)
    # Asymmetry documented in README (#2948 one-liner).
    readme_l = readme.lower()
    if "always-reject" not in readme and "always reject" not in readme_l:
        fails.append("AC3: README must document spec-0 vs policy-0 asymmetry")

    # AC4
    must("load_mailbox_bp_recent", "AC4", spawn)
    must("load_mailbox_bp_recent", "AC4", scope)
    must("2948 AC4", "AC4", test_admit)

    # AC5
    must("bp_threshold_resolve_total", "AC5", spawn)
    must("schema-2948", "AC5", prim)
    must("bp-threshold-resolve-total", "AC5", prim)
    must("bp-threshold-ssot-wired", "AC5", prim)
    must("kBpThresholdSsotIssue", "AC5", spawn)
    must("schema-2887", "AC5", prim)

    # AC6
    must("check_bp_threshold_ssot_2948", "AC6", build)
    must("2948", "AC6", test_admit)
    must("2948", "AC6", test_pol)
    if (ROOT / "tests" / "orch" / "test_issue_2948.cpp").is_file():
        fails.append("AC6: test_issue_2948.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2948-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Call sites actually invoke SSOT (not just declare).
    if "thr_d = resolve_bp_threshold" not in spawn and "resolve_bp_threshold(" not in spawn:
        fails.append("AC6: spawn admit must call resolve_bp_threshold")
    if "thr_d = resolve_bp_threshold" not in scope and "resolve_bp_threshold(" not in scope:
        fails.append("AC6: watch_all must call resolve_bp_threshold")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2948 BP threshold SSOT (spawn admit + watch degrade)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
