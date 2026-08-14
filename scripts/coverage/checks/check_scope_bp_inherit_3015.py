#!/usr/bin/env python3
"""Issue #3015: production AgentScope auto-inherits bp_scope_id.

Contract (one row per AC):
  AC1  Documented: empty bp_scope_id under production AgentScope
       auto-inherits a session-local as:<seq> key. README +
       kBpScopeProcessBucket ("-") sentinel. Additive schema-3015.
  AC2  Soft / sandbox=off / explicit id / "-" stay process-bucket
       (no inherit; no extra atomic on Soft empty path).
  AC3  Two AgentScopes under production: storm in A does not bump
       spawn_bp_admit_reject for B. Existing mailbox_bp_admit +
       per_scope_bp_admit tests stay green.
  AC4  No process-global AgentRegistry; key lives on the scope object.
  AC5  Extend test_per_scope_bp_admit (#81967); no test_issue_3015.cpp;
       no docs/design/ (#1655).

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

    spawn = _read("src/orch/agent_spawn.h")
    scope = _read("src/orch/agent_scope.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    test = _read("tests/orch/test_per_scope_bp_admit.cpp")
    build = _read("build.py")

    # ── AC1: documented inherit ────────────────────────────────────
    must("Issue #3015", "AC1", spawn)
    must("Issue #3015", "AC1", scope)
    must("kBpScopeProcessBucket", "AC1", spawn)
    must("kBpScopeInheritIssue", "AC1", spawn)
    must("production_scope_bp_inherit", "AC1", scope)
    must("make_agent_scope_bp_id", "AC1", scope)
    must("as:", "AC1", scope)
    must("spawn_bp_scope_inherited_total", "AC1", spawn)
    must("spawn_bp_process_bucket_used_total", "AC1", spawn)
    must("schema-3015", "AC1", agent)
    must("Issue #3015", "AC1", readme)
    must("as:<seq>", "AC1", readme)

    # ── AC2: Soft / explicit process bucket ────────────────────────
    must("sandbox=off", "AC2", scope)
    must("AURA_SANDBOX", "AC2", scope)
    must("kBpScopeProcessBucket", "AC2", spawn)
    if "scope_id == kBpScopeProcessBucket" not in spawn:
        fails.append("AC2: spawn must treat '-' as process bucket")
    must("production_reclaimed_must_wait", "AC2", spawn)

    # ── AC3: inherit only when empty + production ──────────────────
    must("spec.bp_scope_id.empty() && production_scope_bp_inherit()", "AC3", scope)
    must("#3015 AC3: B admits while A + process bucket are hot", "AC3", test)

    # ── AC4: no registry ───────────────────────────────────────────
    mark = scope.find("Issue #3015")
    if mark >= 0 and "AgentRegistry" in scope[mark : mark + 900]:
        fails.append("AC4: #3015 must not introduce AgentRegistry")
    must("session-local", "AC4", scope)

    # ── AC5: tests + no invent + no docs/design/ ───────────────────
    must("#3015 AC1", "AC5", test)
    must("check_scope_bp_inherit_3015", "AC5", build)
    for rel in (
        "tests/orch/test_issue_3015.cpp",
        "tests/compiler/test_issue_3015.cpp",
        "tests/serve/test_issue_3015.cpp",
    ):
        if _read(rel):
            fails.append(f"AC5: {rel} exists — forbidden per #81967")
    for rel in (
        "docs/design/3015-bp-scope-inherit.md",
        "docs/design/3015-*.md",
    ):
        if _read(rel):
            fails.append(f"AC5: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #3015 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3015 scope bp_scope_id inherit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
