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


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _find_function_bodies(text: str) -> list[tuple[int, str]]:
    """Return (start_line, body) for each function/lambda body found.

    Naive regex matches `name(...) {` or `[](...) {` / `auto name(...) {`.
    Uses brace-depth walker to capture the full body (handles nested
    braces, lambdas inside functions, etc.).
    """
    bodies: list[tuple[int, str]] = []
    # Match function header at the start of a line. C++ allows
    # complex return types / qualifiers / attributes, so we use a
    # permissive pattern: anything ending with `(...) {`.
    # Restrict to lines that look like function definitions (have a `(`).
    for lineno, line in enumerate(text.splitlines(), start=1):
        # Cheap pre-filter: must contain a `(` before a `{` on the same line
        # (function signature) AND `{` to start the body.
        if "{" not in line or "(" not in line:
            continue
        # Find the opening `{` on this line.
        brace_pos = line.find("{")
        if brace_pos == -1:
            continue
        # Walk forward from brace_pos+1 to capture the body until matching `}`.
        depth = 1
        i = brace_pos + 1
        body_start = i
        while i < len(text) and depth > 0:
            c = text[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        if depth == 0:
            body = text[body_start : i - 1]
            # Only include bodies that are non-trivial (>= 20 chars).
            if len(body) >= 20:
                bodies.append((lineno, body))
    return bodies


def _check_body(body: str) -> tuple[bool, str]:
    """Return (violation, reason) for a function body.

    Triggers when body has:
      - StableNodeRef in scope (parameter or local)
      - require_effect( call (3-arg positional form)
      - AND no `ref_tenant` in scope
      - AND no `require_effect_on_ref(` call
    """
    has_stable_node_ref = "StableNodeRef" in body
    has_require_effect = bool(re.search(r"\brequire_effect\s*\(", body))
    if not (has_stable_node_ref and has_require_effect):
        return False, ""
    has_ref_tenant = "ref_tenant" in body
    has_require_effect_on_ref = bool(
        re.search(r"\brequire_effect_on_ref\s*\(", body)
    )
    if has_ref_tenant or has_require_effect_on_ref:
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
            f"\n{len(fails)} contract row(s) failed "
            f"({violations} AC5 violation(s) found in scope files)",
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