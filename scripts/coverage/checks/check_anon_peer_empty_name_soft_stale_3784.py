#!/usr/bin/env python3
"""Issue #3784: empty-name / anonymous peer closures leave native under hard OS.

#3300 name soft-stale returns 0 for !name||!*name unless #3514 overflow.
Peer anonymous closures of an owner-scoped hard-invalidated define could
keep cached native while named peers MustDeopt.

#3784: aura_closure_call empty-name path consults #3750 peer AOT slot
soft_stale via closure func_id / stable sid. Soft/Off unchanged
(multi-eval mark already gated; Soft may all-slot).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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
            fails.append(f"{label}: forbidden {n!r}")

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    test = _read("tests/compiler/test_peer_jit_name_soft_stale.cpp")
    build = _read("build.py")

    must("Issue #3784", "AC1 runtime cite", rt)
    must("aura_aot_slot_is_soft_stale", "AC1 slot consult", rt)
    must("peer_leave_native", "AC1 leave-native flag", rt)
    must("peer_cname == nullptr", "AC1 empty-name branch", rt)

    must("aura_aot_peer_jit_name_is_soft_stale(peer_cname)", "AC2 named path", rt)

    must("Issue #3784", "AC3 bridge cite", br)
    must("aura_aot_soft_stale_peer_slots_for_name", "AC3 #3750 retained", br)

    must("ac3784_1", "AC4 test", test)
    must("ac3784_2", "AC4 test", test)
    must("ac3784_3", "AC4 test", test)
    must("check_anon_peer_empty_name_soft_stale_3784", "AC4 build wire", build)
    if build.find("check_anon_peer_empty_name_soft_stale_3784") <= build.find(
        "check_auto_arm_densify_health_publish_3783"
    ):
        fails.append("AC4: linter must be wired AFTER #3783 linter")
    if (ROOT / "tests" / "issues" / "test_issue_3784.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3784.cpp present")
    if (ROOT / "tests" / "compiler" / "test_issue_3784.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3784.cpp present")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    print("OK  AC1_empty_name_func_id_soft_stale")
    print("OK  AC2_named_3300_retained")
    print("OK  AC3_3750_slot_mark_retained")
    print("OK  AC4_tests_wiring")
    print(
        "\nOK: Issue #3784 empty-name peer leave-native — func_id soft_stale "
        "consult; named #3300 retained; Soft/Off unchanged"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
