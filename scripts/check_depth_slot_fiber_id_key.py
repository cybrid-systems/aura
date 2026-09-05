#!/usr/bin/env python3
"""Issue #3552 linter: enforce that all callers of Evaluator::mutation_boundary_depth_slot
pass a fiber_id argument (either a captured fiber_id_ member or a derived `fid` from
g_current_fiber). Legacy single-arg calls without fiber_id are refused because they
silently collapse to the legacy single-slot-per-instance path — exactly the cross-worker
hot-swap leak #3552 closes.

Usage:
    python3 scripts/check_depth_slot_fiber_id_key.py --strict

Scope (source-of-truth files only):
    - src/compiler/evaluator.ixx            (declaration)
    - src/compiler/evaluator_fiber_mutation.cpp (implementation + 3 callers)
    - src/compiler/evaluator_mutation_boundary.cpp (MutationBoundaryGuard ctor/dtor/move)

Forbidden patterns (post-#3552):
    - mutation_boundary_depth_slot(ev)        (no fiber_id arg)  → production multi-fiber EDEADLK
    - mutation_boundary_depth_slot(ev_)       (no fiber_id arg)  → dtor under-decrement
    - mutation_boundary_depth_slot(this)      (no fiber_id arg)  → invariant probe wrong slot

Required patterns:
    - mutation_boundary_depth_slot(Evaluator* ev, std::uint64_t fiber_id)         (ixx decl)
    - mutation_boundary_depth_slot(ev, fiber_id_)  /  ...(ev, fid)  /  ...(this, fid)  (callers)
    - dtor reads ctor-captured fiber_id_ (no fresh g_current_fiber lookup)
    - move ctor copies fiber_id_
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "compiler"

REQUIRED_FILES = [
    SRC / "evaluator.ixx",
    SRC / "evaluator_fiber_mutation.cpp",
    SRC / "evaluator_mutation_boundary.cpp",
]

# Forbidden: 1-arg call to mutation_boundary_depth_slot (production / hot path).
# Matches `mutation_boundary_depth_slot(ev)` / `(ev_)` / `(this)` /
# `(ev, )` / `(ev_)` etc. — single argument only.
RE_FORBIDDEN_CALL = re.compile(
    r"\bmutation_boundary_depth_slot\s*\(\s*[^,)\n]+\s*\)",
    re.MULTILINE,
)
# Required: declaration in evaluator.ixx carries fiber_id.
RE_REQUIRED_DECL = re.compile(
    r"\bmutation_boundary_depth_slot\s*\(\s*Evaluator\s*\*\s*\w+\s*,\s*std::uint64_t\s+\w+\s*\)",
)


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument(
        "--strict", action="store_true", help="fail on any forbidden 1-arg call to mutation_boundary_depth_slot"
    )
    p.parse_args()

    violations = 0

    for f in REQUIRED_FILES:
        if not f.exists():
            fail(f"missing required file: {f}")
            violations += 1
            continue
        text = f.read_text(encoding="utf-8", errors="replace")

        # 1. declaration site (evaluator.ixx) carries fiber_id
        if f.name == "evaluator.ixx":
            if not RE_REQUIRED_DECL.search(text):
                fail(
                    f"{f}: declaration missing fiber_id arg (expected "
                    f"`mutation_boundary_depth_slot(Evaluator* ev, std::uint64_t fiber_id)`)"
                )
                violations += 1
            # forbid 1-arg declaration form (single-arg overload)
            for m in RE_FORBIDDEN_CALL.finditer(text):
                fail(f"{f}: {m.start()}: forbidden 1-arg call: {text[m.start() : m.end()][:120]}")
                violations += 1
            continue

        # 2. callers (cpp) must pass 2 args; no 1-arg mutation_boundary_depth_slot call.
        for m in RE_FORBIDDEN_CALL.finditer(text):
            snippet = text[m.start() : m.end()][:120]
            # Allow exceptions: line starting with `//` (comment), or inside a doc comment.
            line_start = text.rfind("\n", 0, m.start()) + 1
            line_prefix = text[line_start : m.start()]
            if line_prefix.lstrip().startswith("//"):
                continue
            fail(f"{f}:{m.start()}: forbidden 1-arg call: {snippet}")
            violations += 1

        # 3. ctor must capture fiber_id from g_current_fiber
        if f.name == "evaluator_mutation_boundary.cpp":
            if "fiber_id_ = (aura::serve::g_current_fiber" not in text:
                fail(f"{f}: ctor must capture fiber_id from g_current_fiber")
                violations += 1
            if ", fiber_id_(o.fiber_id_)" not in text:
                fail(f"{f}: move ctor must propagate fiber_id_")
                violations += 1

        # 4. impl must use nested unordered_map keyed by (instance_id, fiber_id)
        if f.name == "evaluator_fiber_mutation.cpp":
            if "auto& inner = slot->depths[id];" not in text:
                fail(f"{f}: depth_slot impl must use nested map keyed by (instance_id, fiber_id)")
                violations += 1
            if "inner.emplace(fiber_id, 0)" not in text:
                fail(f"{f}: depth_slot impl must emplace per-(instance, fiber) entry initialized to 0")
                violations += 1

    if violations > 0:
        print(f"\n{lint_name()}: {violations} violation(s) — refusing to ship", file=sys.stderr)
        return 1
    print(f"{lint_name()}: OK (3552 AC: fiber_id dimension enforced)")
    return 0


def lint_name() -> str:
    return "check_depth_slot_fiber_id_key"


if __name__ == "__main__":
    sys.exit(main())
