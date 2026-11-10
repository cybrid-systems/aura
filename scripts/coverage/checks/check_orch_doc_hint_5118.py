#!/usr/bin/env python3
"""Issue #5118: restored orch verbs stay registered, and the deprecation hint
names a function that is registered.

PrimMeta.doc for every non-kernel orch: name is built in
mark_retired_surfaces. The hint must say DEPRECATED (#5118) and name either
that verb or parallel-intend (the body orch:parallel-intend aliases).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

RESTORED = (
    "orch:agent-ask",
    "orch:agent-directory",
    "orch:agent-export-via-token",
    "orch:agent-import-via-token",
    "orch:agent-join",
    "orch:agent-poll",
    "orch:agent-recv",
    "orch:agent-reply",
    "orch:agent-send",
    "orch:agent-touch",
    "orch:agent-wait-reclaimed",
    "orch:compose-workflow",
    "orch:join-via-token",
    "orch:metrics",
    "orch:run-workflow",
    "orch:scope-cancel-all",
    "orch:scope-child",
    "orch:scope-join-all",
    "orch:scope-resolve",
    "orch:scope-spawn",
    "orch:scope-sweep-reclaimed-pending",
    "orch:scope-watch",
    "orch:supervise-batch",
)

ADD_RE = re.compile(r'^\s*add(?:_raw)?\(\s*"([^"]+)"', re.MULTILINE)


def live_adds(text: str) -> set[str]:
    """add() calls whose previous line is not `if (false)`."""
    found = set()
    for match in ADD_RE.finditer(text):
        line_start = text.rfind("\n", 0, match.start()) + 1
        prev_end = line_start - 1
        prev_start = text.rfind("\n", 0, max(prev_end, 0)) + 1
        if text[prev_start:prev_end].strip() == "if (false)":
            continue
        found.add(match.group(1))
    return found


def main() -> int:
    fails: list[str] = []
    agent = (ROOT / "src/compiler/evaluator_primitives_agent.cpp").read_text(encoding="utf-8")
    messaging = (ROOT / "src/compiler/evaluator_primitives_messaging.cpp").read_text(encoding="utf-8")
    registry = (ROOT / "src/compiler/evaluator_primitives_registry.cpp").read_text(encoding="utf-8")
    live = live_adds(agent) | live_adds(messaging)
    # Issue #5131: a name on this list is satisfied either by its kernel
    # registration or by a definition in lib/std/orch (the migration target).
    # The list stays at 24 so a migrated name cannot silently disappear from
    # both homes.
    orch_mod = (ROOT / "lib/std/orch.aura").read_text(encoding="utf-8")
    for name in RESTORED:
        if name in live:
            continue
        if f"(define ({name}" in orch_mod:
            continue
        fails.append(f"{name} is neither a live add() nor defined in std/orch")
    if "parallel-intend" not in live_adds(agent):
        fails.append("parallel-intend is not registered, so the alias hint has no target")
    if "DEPRECATED (#5118)" not in registry:
        fails.append("registry does not build a #5118 PrimMeta.doc hint")
    if "std::string(view)" not in registry:
        fails.append("other orch hints do not name the registered verb")
    if fails:
        print("FAIL #5118 orch doc hints:")
        for item in fails:
            print(f"  - {item}")
        return 1
    print(f"OK #5118 orch doc hints: {len(RESTORED)} verbs registered, hint names a live function")
    return 0


if __name__ == "__main__":
    sys.exit(main())
