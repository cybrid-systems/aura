#!/usr/bin/env python3
"""Issue #2953: Agent recovery playbook query (single action from snapshot).

Refine #2367/#2302 — pure observe-only recommended action enum so Agents
branch recovery without multi-key OR. Soft empty → idle.

Contract (one row per AC):
  AC1  controlled snapshots match decision table
  AC2  idle → action Idle
  AC3  decide is observe-only (no reemit/drain/reload)
  AC4  schema-2953 additive; recovery keys preserved
  AC5  Soft idle path green
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

    hh = _read("src/compiler/hot_update_registry.hh")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2953", "AC1", hh)
    must("ReloadRecoveryPlaybookAction", "AC1", hh)
    must("aura_reload_recovery_playbook_decide", "AC1", hh)
    must("aura_reload_recovery_playbook_decide", "AC1", cpp)
    must("RejectCrossWs", "AC1", hh)
    must("WaitStorm", "AC1", hh)
    must("ForceDrain", "AC1", hh)
    must("2953 AC1", "AC1", test)

    # AC2
    must("Idle", "AC2", hh)
    must("2953 AC2", "AC2", test)

    # AC3 observe-only decide body
    start = cpp.find("aura_reload_recovery_playbook_decide")
    if start < 0:
        fails.append("AC3: decide definition missing")
    else:
        body = cpp[start : start + 1600]
        if "aura_reemit_aot_for_dirty" in body:
            fails.append("AC3: decide calls reemit")
        if "drain_pending_recovery" in body:
            fails.append("AC3: decide calls drain")
        if "aura_reload_aot" in body:
            fails.append("AC3: decide calls reload")
    must("2953 AC3", "AC3", test)

    # AC4
    must("schema-2953", "AC4", mut)
    must("issue-2953", "AC4", mut)
    must("playbook-action", "AC4", mut)
    must("playbook-wired", "AC4", mut)
    must("query:reload-recovery-playbook", "AC4", mut)
    must("schema-2367", "AC4", mut)
    must("schema_2953", "AC4", hh)

    # AC5 / AC6
    must("ac2953_playbook", "AC6", test)
    must("check_reload_recovery_playbook_2953", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2953.cpp").is_file():
        fails.append("AC6: test_issue_2953.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2953-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Decision table comment present
    must("reject-cross-ws", "AC1", hh)
    must("wait-storm", "AC1", hh)
    must("force-drain", "AC1", hh)
    must("retry-reload", "AC1", hh)
    must("fall-back-jit", "AC1", hh)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2953 reload recovery playbook")
    return 0


if __name__ == "__main__":
    sys.exit(main())
