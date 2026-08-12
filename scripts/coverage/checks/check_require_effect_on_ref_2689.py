#!/usr/bin/env python3
"""Issue #2689: mandate require_effect_on_ref on all StableNodeRef side-effect paths.

Contract:
  AC1 Inventory of side-effect prims holding StableNodeRef — each routes
      through require_effect_on_ref or explicit ref_tenant. Source-cite
      table in test (test_require_effect_auto_isolation.cpp). The
      require_effect(re…, target_node, ref_tenant=0) three-arg form is
      NodeId-only and stays allowed on pure NodeId APIs.
  AC2 Foreign ref under Restricted/Strict → deny before mutate body;
      capability_fiber_hard_deny_total + isolation deny bump; no
      effect-allow SE for that attempt (covered by #2658 baseline).
  AC3 Same-tenant ref + grant → allow (no false deny, covered by #2658).
  AC4 mutate:force #2658 pattern remains; regression tests green.
  AC5 Coverage linter flags require_effect( inside a function/lambda
      that also names a StableNodeRef parameter/local without ref_tenant /
      require_effect_on_ref. Allowlist only documented NodeId-only paths.
  AC6 Source-cite + coverage linter; extend test_require_effect_auto_isolation
      / test_mutate_capability_force per #81967 (no docs/design per #1655).

This linter (AC5) scans evaluator_primitives*.cpp + evaluator_security.cpp
for functions/lambdas that:
  - declare or name a StableNodeRef parameter/local in scope
  - call require_effect( with positional target_node (3-arg form)
  - but do NOT name ref_tenant in the same body AND do NOT call
    require_effect_on_ref( anywhere in the body

Such bodies are a residual late-isolation window: a foreign-tenant
ref would pass the capability gate and partially enter the body before
resolve_stamped / a later isolation check fails (the exact window #2658
closed for mutate:force).

Exit 0 = OK, 1 = violation found.
"""

from __future__ import annotations

import bisect
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
# Files in scope: side-effect primitives + security core.
SCOPE_FILES = [
    "src/compiler/evaluator_security.cpp",
    "src/compiler/evaluator_primitives_mutate.cpp",
    "src/compiler/evaluator_primitives_compile.cpp",
    "src/compiler/evaluator_primitives_runtime.cpp",
    "src/compiler/evaluator_primitives_io.cpp",
    "src/compiler/evaluator_primitives_messaging.cpp",
]

# Control-flow openers that share `... ( ... ) {` shape with functions.
_CTRL_OPEN = re.compile(r"^\s*(?:if|else\s+if|else|for|while|switch|catch|try|do)\b")
# require_effect( but not require_effect_on_ref(
_REQUIRE_EFFECT_CALL = re.compile(r"\brequire_effect(?!_on_ref)\s*\(")


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _find_function_bodies(text: str) -> list[tuple[int, str]]:
    """Return (start_line, body) for function/lambda-like `{...}` regions.

    Single linear scan (O(n)) with a brace stack. Previous implementation
    treated each line's brace offset as a file index and re-walked the whole
    file per candidate line — ~O(lines × bytes) and the main gate bottleneck.

    Semantics preserved for AC5: a body is checked if it names StableNodeRef
    and calls require_effect(; nested lambdas are covered because their text
    sits inside the enclosing function body.
    """
    if not text or "StableNodeRef" not in text or "require_effect" not in text:
        return []

    line_starts = [0]
    for i, ch in enumerate(text):
        if ch == "\n":
            line_starts.append(i + 1)

    def line_no(pos: int) -> int:
        return bisect.bisect_right(line_starts, pos)

    bodies: list[tuple[int, str]] = []
    stack: list[tuple[int, int, bool]] = []  # (open_pos, line, is_fn_like)
    n = len(text)
    i = 0
    while i < n:
        c = text[i]
        if c == "{":
            ln = line_no(i)
            ls = line_starts[ln - 1]
            le = text.find("\n", ls)
            line = text[ls : le if le >= 0 else n]
            local = i - ls
            # Function / lambda opener: `(` before `{` on this line, not if/for/...
            is_fn = "(" in line[:local] and not _CTRL_OPEN.match(line)
            stack.append((i, ln, is_fn))
        elif c == "}" and stack:
            open_pos, ln, is_fn = stack.pop()
            if is_fn:
                body = text[open_pos + 1 : i]
                # Cheap prefilter: only keep bodies that can violate AC5.
                if len(body) >= 20 and "StableNodeRef" in body and "require_effect" in body:
                    bodies.append((ln, body))
        i += 1
    return bodies


def _check_body(body: str) -> tuple[bool, str]:
    """Return (violation, reason) for a function body.

    Triggers when body has:
      - StableNodeRef in scope (parameter or local)
      - require_effect( call (3-arg positional form)
      - AND no `ref_tenant` in scope
      - AND no `require_effect_on_ref(` call
    """
    if "StableNodeRef" not in body:
        return False, ""
    if not _REQUIRE_EFFECT_CALL.search(body):
        return False, ""
    if "ref_tenant" in body or "require_effect_on_ref" in body:
        return False, ""
    return True, (
        "function/lambda has StableNodeRef in scope and calls "
        "require_effect( but does NOT name ref_tenant / require_effect_on_ref; "
        "foreign-tenant ref will pass capability gate and partially run before "
        "late isolation deny (#2658 window). Use require_effect_on_ref(ref) or "
        "pass ref.tenant_id as the 4th argument."
    )


def main() -> int:
    fails: list[str] = []

    # AC6 — Issue #2689 sentinel in evaluator_security.cpp + require_effect
    # declaration + require_effect_on_ref definition all present.
    sec = _read("src/compiler/evaluator_security.cpp")
    if "Issue #2689" not in sec:
        fails.append("AC6: Issue #2689 sentinel missing in evaluator_security.cpp")
    if "require_effect_on_ref" not in sec:
        fails.append("AC6: require_effect_on_ref definition missing")
    if "ref_tenant" not in sec:
        fails.append("AC6: ref_tenant parameter missing")

    # AC5 — Coverage linter scan.
    violations = 0
    for rel in SCOPE_FILES:
        text = _read(rel)
        if not text:
            continue
        bodies = _find_function_bodies(text)
        for start_line, body in bodies:
            is_violation, reason = _check_body(body)
            if is_violation:
                violations += 1
                fails.append(f"AC5: {rel}:{start_line}: {reason}")

    # AC6 — no docs/design/* per #1655.
    for rel in (
        "docs/design/require_effect_on_ref_2689.md",
        "docs/require_effect_on_ref_2689.md",
        "design/2689.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # Linter file on disk.
    linter_path = ROOT / "scripts/coverage/checks/check_require_effect_on_ref_2689.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} contract row(s) failed ({violations} AC5 violation(s) found in scope files)",
            file=sys.stderr,
        )
        return 1
    print(
        f"OK: Issue #2689 require_effect_on_ref coverage — all AC rows satisfied "
        f"({violations} AC5 violation(s) found in scope files)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
