#!/usr/bin/env python3
"""Issue #3437: ~Evaluator must drop the session AgentScope.

Two session-local identity stores existed, only one was torn down with
the Evaluator: Evaluator::agent_names_ (drained by cleanup_orch_agents)
and g_evaluator_agent_scopes()[ev*] (only explicit orch:scope-join-all
drop-if-empty or test reset). A host that scope-spawns and destroys the
Evaluator without join-all left a unique_ptr AgentScope keyed by a
dangling Evaluator* — fiber / mailbox / reservation / Scheduler-observer
leak until process exit, and an address-recycled Evaluator could inherit
a foreign scope tree.

Fix: cleanup_orch_agents ends with drop_agent_scope on this — the
~AgentScope cancel + drain + reservation release runs; the erase is
unconditional session cleanup (Soft/Off same path) and idempotent with
join-all drop-if-empty.

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

    ctor = _read("src/compiler/evaluator_ctor.cpp")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_orch_scope.cpp")
    build = _read("build.py")

    # AC1: the drop call lives INSIDE cleanup_orch_agents (window check,
    # not file-wide — per source-cite windowing rule).
    fn_idx = ctor.find("void Evaluator::cleanup_orch_agents() noexcept")
    if fn_idx < 0:
        fails.append("AC1: cleanup_orch_agents signature not found")
    else:
        snip = ctor[fn_idx : fn_idx + 2600]
        must("Issue #3437", "AC1 cite", snip)
        must("drop_agent_scope(static_cast<void*>(this))", "AC1 drop call", snip)
        # AC3: unconditional session cleanup — no production/sandbox gate.
        for bad in ("sandbox_mode_", "effect_sandbox_mode()", "production_defaults_active"):
            if bad in snip:
                fails.append(f"AC3: drop must be unconditional, found gate {bad!r}")
    must('#include "orch/agent_scope.h"', "AC1 include", ctor)
    must("leak-3437", "AC1 dtor soak test", test)
    must("~Evaluator erased the scope map slot", "AC1 check label", test)

    # AC2: recycled-address placement-new test present.
    must("addr-b-3437", "AC2 recycled spawn", test)
    must("recycled address inherits no foreign scope", "AC2 check label", test)

    # AC4: orch:scope-join-all drop-if-empty stays (idempotent erase).
    ja = prim.find('add("orch:scope-join-all"')
    if ja < 0:
        fails.append("AC4: orch:scope-join-all prim not found")
    else:
        must("drop_agent_scope", "AC4 drop-if-empty intact", prim[ja : ja + 8000])

    # AC5: linter wired in build.py AFTER #3434; no test_issue files;
    # no docs/design.
    must("check_scope_session_drop_3437", "AC5 build.py", build)
    prev = build.find("check_tenant_spawn_mandate_3434")
    ours = build.find("check_scope_session_drop_3437")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3434")
    for forbidden in ("tests/orch/test_issue_3437.cpp", "tests/issues/test_issue_3437.cpp"):
        if (ROOT / forbidden).is_file():
            fails.append(f"AC5: {forbidden} present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3437-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    # AC6: no AgentRegistry surface added by the teardown change.
    if "AgentRegistry" in ctor:
        fails.append("AC6: evaluator_ctor.cpp mentions AgentRegistry (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3437 scope session drop — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
