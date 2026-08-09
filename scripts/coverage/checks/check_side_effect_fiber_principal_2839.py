#!/usr/bin/env python3
"""Issue #2839: residual side-effect + fiber-entry principal enforcement.

Contract (one row per AC):
  AC1 Inventory of NodeId-only side-effect entries + require_effect_for_node_id
  AC2 Foreign-tenant stamped path still require_effect_on_ref (late isolation closed)
  AC3 Fiber resume mismatch under production → hard counter + SE + re-bind
  AC4 Existing #2689 / #2659 / #2491 surfaces preserved
  AC5 Expanded coverage linter (more prim files) + 0 new StableNodeRef violations
  AC6 Source-cite + tests; no docs/design/; no invent test_issue_2839.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Expanded scope beyond #2689 — residual NodeId / side-effect prim files.
SCOPE_FILES = [
    "src/compiler/evaluator_security.cpp",
    "src/compiler/evaluator_primitives_mutate.cpp",
    "src/compiler/evaluator_primitives_mutation.cpp",
    "src/compiler/evaluator_primitives_compile.cpp",
    "src/compiler/evaluator_primitives_runtime.cpp",
    "src/compiler/evaluator_primitives_io.cpp",
    "src/compiler/evaluator_primitives_messaging.cpp",
    "src/compiler/evaluator_primitives_file.cpp",
    "src/compiler/evaluator_primitives_security.cpp",
    "src/compiler/evaluator_primitives_agent.cpp",
    "src/compiler/evaluator_fiber_mutation.cpp",
]

# Documented exempt 2-arg require_effect ops (non-workspace NodeId).
# AC1 inventory: these are allowed without require_effect_for_node_id.
EXEMPT_2ARG_OPS = {
    "write-file",  # filesystem, not workspace node
    "mutation-log-compact",  # log maintenance, no NodeId target
    "security:check-effect",  # capability probe, not mutate body
}


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _check_stable_ref_body(body: str) -> bool:
    """True if body violates #2689 StableNodeRef + bare require_effect."""
    if "StableNodeRef" not in body:
        return False
    if not re.search(r"\brequire_effect\s*\(", body):
        return False
    if "ref_tenant" in body or re.search(r"\brequire_effect_on_ref\s*\(", body):
        return False
    return not re.search(r"\brequire_effect_for_node_id\s*\(", body)


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    sec = _read("src/compiler/evaluator_security.cpp")
    fiber_mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fiber_h = _read("src/serve/fiber.h")
    fiber_cpp = _read("src/serve/fiber.cpp")
    compile_cpp = _read("src/compiler/evaluator_primitives_compile.cpp")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    test_re = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    test_ts = _read("tests/compiler/test_tenant_scope_fiber_mandate.cpp")
    build = _read("build.py")
    linter_2689 = _read("scripts/coverage/checks/check_require_effect_on_ref_2689.py")

    # AC1 — require_effect_for_node_id + inventory of NodeId path
    must("Issue #2839", "AC1", sec)
    must("require_effect_for_node_id", "AC1", sec)
    must("make_stamped_ref", "AC1", sec)
    must("require_effect_for_node_id", "AC1", compile_cpp)
    must("mutate:from-verification-feedback", "AC1", compile_cpp)

    # AC2 — on_ref path preserved
    must("require_effect_on_ref", "AC2", sec)
    must("ref_tenant", "AC2", sec)

    # AC3 — fiber principal hard face
    must("Issue #2839", "AC3", fiber_mut)
    must("bump_tenant_scope_mismatch_hard", "AC3", fiber_h)
    must("static_tenant_scope_mismatch_hard_total_", "AC3", fiber_h)
    must("static_tenant_scope_mismatch_hard_total_", "AC3", fiber_cpp)
    must("isolation-deny:fiber-principal-mismatch", "AC3", fiber_mut)
    must("production_defaults_active()", "AC3", fiber_mut)
    must("TenantScope", "AC3", fiber_mut)

    # AC4 — #2689 / #2491 lineage
    must("Issue #2689", "AC4", sec)
    must("Issue #2491", "AC4", fiber_mut)
    must("check_require_effect_on_ref_2689", "AC4", linter_2689)

    # AC5 — expanded scope: per-body #2689-style scan (StableNodeRef + bare
    # require_effect without ref_tenant / on_ref / for_node_id).
    violations = 0
    for rel in SCOPE_FILES:
        text = _read(rel)
        if not text:
            continue
        # Split on function-ish braces: walk top-level-ish chunks with StableNodeRef.
        # Reuse a lightweight body walker matching #2689.
        pos = 0
        while True:
            idx = text.find("StableNodeRef", pos)
            if idx < 0:
                break
            # Expand a window around the mention (±2k chars) as a pseudo-body.
            lo = max(0, idx - 800)
            hi = min(len(text), idx + 2000)
            window = text[lo:hi]
            # Confirm bare require_effect without safe form (on_ref / for_node_id /
            # ref_tenant). Flatten nested ifs for ruff SIM102.
            if (
                _check_stable_ref_body(window)
                and re.search(r"\brequire_effect\s*\(", window)
                and not re.search(
                    r"\brequire_effect_on_ref\s*\(|\brequire_effect_for_node_id\s*\(",
                    window,
                )
                and "ref_tenant" not in window
            ):
                violations += 1
                fails.append(
                    f"AC5: {rel}: StableNodeRef vicinity has bare require_effect without on_ref/for_node_id/ref_tenant"
                )
                break  # one fail per file is enough
            pos = idx + 13
    # AC5 also requires expanded scope list includes mutate + fiber files.
    must("evaluator_primitives_mutate.cpp", "AC5", "\n".join(SCOPE_FILES))
    must("evaluator_fiber_mutation.cpp", "AC5", "\n".join(SCOPE_FILES))

    # AC5 — linter + query surface
    must("schema-2839", "AC5", posture)
    must("tenant-scope-mismatch-hard-total", "AC5", posture)
    must("require-effect-for-node-id-wired", "AC5", posture)
    must("check_side_effect_fiber_principal_2839", "AC5", build)

    # AC6 — tests + no invent/design
    must("2839", "AC6", test_ts)
    must("2839", "AC6", test_re)
    if (ROOT / "tests" / "compiler" / "test_issue_2839.cpp").is_file():
        fails.append("AC6: test_issue_2839.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2839-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check #2689 still green
    r = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_require_effect_on_ref_2689.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"check_require_effect_on_ref_2689 regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2839 side-effect + fiber principal residual enforcement")
    return 0


if __name__ == "__main__":
    sys.exit(main())
