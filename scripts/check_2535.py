#!/usr/bin/env python3
"""Issue #2535: production default mild mailbox BP admit (threshold=32)."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    h = _read("src/orch/agent_spawn.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    test = _read("tests/orch/test_mailbox_bp_admit_default_2535.cpp")

    must("kMailboxBpAdmitThresholdDefault = 32", "AC1", h)
    must("2535", "AC1", h)
    must("kMailboxBpAdmitDefaultOnIssue", "AC1", h)
    must("AURA_ORCH_BP_ADMIT_THRESHOLD", "AC2", h)
    must("schema-2535", "AC5", agent)
    must("mailbox-bp-admit-threshold-default", "AC5", agent)
    must("2535", "AC5", readme)
    must("test_mailbox_bp_admit_default_2535", "AC6", cmake)
    must("aura_issue_test_link_light(test_mailbox_bp_admit_default_2535)", "AC6", cmake)
    must("check_2535", "AC6", build)
    must("AC1", "AC6", test)

    if fails:
        print("check_2535: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_2535: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
