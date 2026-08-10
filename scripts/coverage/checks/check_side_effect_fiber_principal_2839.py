#!/usr/bin/env python3
"""Issue #2839 + #2881: residual side-effect + fiber-entry principal enforcement.

Contract (one row per AC):
  AC1  Inventory of NodeId-only side-effect entries + require_effect_for_node_id (#2839)
  AC2  Foreign-tenant stamped path still require_effect_on_ref (late isolation closed) (#2839)
  AC3  Fiber resume mismatch under production → hard counter + SE + re-bind (#2839)
  AC4  Existing #2689 / #2659 / #2491 surfaces preserved (#2839)
  AC5  Expanded coverage linter (more prim files) + 0 new StableNodeRef violations (#2839)
  AC6  Source-cite + tests; no docs/design/; no invent test_issue_2839.cpp (#2839)
  AC7  #2881: residual scope expanded — 6 new prim files in SCOPE_FILES
  AC8  #2881: residual exempt ops — 2 new ops (git-commit, deny_sys) documented
  AC9  #2881: residual inventory constants wired — schema-2881 + 3 constexpr counts match
  AC10 #2881: cross-source-cite — #2881 referenced in evaluator_security.cpp +
       evaluator_primitives_security.cpp + evaluator_primitives_io.cpp
  AC11 #2881: tests added to existing src/-aligned suite; no docs/design/;
       no new test_issue_2881.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Expanded scope beyond #2689 — residual NodeId / side-effect prim files.
# Issue #2839 (11 files) + Issue #2881 (6 new files) = 17 files total.
SCOPE_FILES = [
    # #2839 originals (11)
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
    # #2881 residual coverage (6 new — kResidualNodeIdScopeFilesCount)
    "src/compiler/evaluator_primitives_query_workspace.cpp",
    "src/compiler/evaluator_primitives_diagnostic.cpp",
    "src/compiler/evaluator_primitives_memory.cpp",
    "src/compiler/evaluator_primitives_module.cpp",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "src/compiler/evaluator_primitives_json.cpp",
]

# Documented exempt 2-arg require_effect ops (non-workspace NodeId).
# AC1 inventory (Issue #2839) + AC8 inventory (Issue #2881).
# #2839: 3 entries. #2881 adds 2 (git-commit, deny_sys) = 5 total.
EXEMPT_2ARG_OPS = {
    # #2839 originals (3)
    "write-file",  # filesystem, not workspace node
    "mutation-log-compact",  # log maintenance, no NodeId target
    "security:check-effect",  # capability probe, not mutate body
    # #2881 residual (2)
    "git-commit",  # exec+network (Issue #2072) — no NodeId target
    "deny_sys",  # syscall wrapper (Issue #1329 Phase 1) — cap is string, no NodeId
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
    evaluator_ixx = _read("src/compiler/evaluator.ixx")
    fiber_mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fiber_h = _read("src/serve/fiber.h")
    fiber_cpp = _read("src/serve/fiber.cpp")
    compile_cpp = _read("src/compiler/evaluator_primitives_compile.cpp")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    io_cpp = _read("src/compiler/evaluator_primitives_io.cpp")
    test_re = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    test_ts = _read("tests/compiler/test_tenant_scope_fiber_mandate.cpp")
    build = _read("build.py")
    linter_2689 = _read("scripts/coverage/checks/check_require_effect_on_ref_2689.py")

    # ── #2839 AC1 — require_effect_for_node_id + inventory of NodeId path ──
    must("Issue #2839", "AC1", sec)
    must("require_effect_for_node_id", "AC1", sec)
    must("make_stamped_ref", "AC1", sec)
    must("require_effect_for_node_id", "AC1", compile_cpp)
    must("mutate:from-verification-feedback", "AC1", compile_cpp)

    # ── #2839 AC2 — on_ref path preserved ──
    must("require_effect_on_ref", "AC2", sec)
    must("ref_tenant", "AC2", sec)

    # ── #2839 AC3 — fiber principal hard face ──
    must("Issue #2839", "AC3", fiber_mut)
    must("bump_tenant_scope_mismatch_hard", "AC3", fiber_h)
    must("static_tenant_scope_mismatch_hard_total_", "AC3", fiber_h)
    must("static_tenant_scope_mismatch_hard_total_", "AC3", fiber_cpp)
    must("isolation-deny:fiber-principal-mismatch", "AC3", fiber_mut)
    must("production_defaults_active()", "AC3", fiber_mut)
    must("TenantScope", "AC3", fiber_mut)

    # ── #2839 AC4 — #2689 / #2491 lineage ──
    must("Issue #2689", "AC4", sec)
    must("Issue #2491", "AC4", fiber_mut)
    must("check_require_effect_on_ref_2689", "AC4", linter_2689)

    # ── #2839 AC5 — expanded scope: per-body #2689-style scan (StableNodeRef +
    # bare require_effect without ref_tenant / on_ref / for_node_id).
    violations = 0
    for rel in SCOPE_FILES:
        text = _read(rel)
        if not text:
            continue
        pos = 0
        while True:
            idx = text.find("StableNodeRef", pos)
            if idx < 0:
                break
            lo = max(0, idx - 800)
            hi = min(len(text), idx + 2000)
            window = text[lo:hi]
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

    # ── #2839 AC5 — linter + query surface ──
    must("schema-2839", "AC5", posture)
    must("tenant-scope-mismatch-hard-total", "AC5", posture)
    must("require-effect-for-node-id-wired", "AC5", posture)
    must("check_side_effect_fiber_principal_2839", "AC5", build)

    # ── #2839 AC6 — tests + no invent/design ──
    must("2839", "AC6", test_ts)
    must("2839", "AC6", test_re)
    if (ROOT / "tests" / "compiler" / "test_issue_2839.cpp").is_file():
        fails.append("AC6: test_issue_2839.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2839-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # ── #2881 AC7 — residual scope expanded (6 new files) ──
    ac7_new_files = [
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        "src/compiler/evaluator_primitives_diagnostic.cpp",
        "src/compiler/evaluator_primitives_memory.cpp",
        "src/compiler/evaluator_primitives_module.cpp",
        "src/compiler/evaluator_primitives_obs_jit.cpp",
        "src/compiler/evaluator_primitives_json.cpp",
    ]
    for f in ac7_new_files:
        must(f, "AC7", "\n".join(SCOPE_FILES))
        # Each new file must parse + exist on disk (no dangling entry).
        if not (ROOT / f).is_file():
            fails.append(f"AC7: SCOPE_FILES lists {f} but file missing")

    # ── #2881 AC8 — residual exempt ops (2 new entries documented) ──
    must("git-commit", "AC8", "\n".join(sorted(EXEMPT_2ARG_OPS)))
    must("deny_sys", "AC8", "\n".join(sorted(EXEMPT_2ARG_OPS)))
    # Exempt ops must be reflected in source-cite (io.cpp comments).
    must("git-commit", "AC8", io_cpp)
    must("deny_sys", "AC8", io_cpp)
    must("sys-open", "AC8", io_cpp)  # deny_sys call site
    # Total EXEMPT_2ARG_OPS count matches kResidualNodeIdExemptOpsCount
    # (5: 3 from #2839 + 2 from #2881).
    if len(EXEMPT_2ARG_OPS) != 5:
        fails.append(
            f"AC8: EXEMPT_2ARG_OPS count = {len(EXEMPT_2ARG_OPS)}, "
            f"expected 5 (3 #2839 + 2 #2881) per kResidualNodeIdExemptOpsCount"
        )

    # ── #2881 AC9 — residual inventory constants wired + match ──
    # Constants live in the module interface (evaluator.ixx) so generic
    # lambda template-body two-phase lookup can resolve them across all
    # module TUs. Evaluator_security.cpp may carry a forward reference /
    # comment but the canonical definition is evaluator.ixx.
    must("kResidualNodeIdExemptOpsCount", "AC9", evaluator_ixx)
    must("kResidualNodeIdScopeFilesCount", "AC9", evaluator_ixx)
    must("kResidualNodeIdInventoryCount", "AC9", evaluator_ixx)
    # Constants must equal the actual counts (drift trips the linter).
    m_exempt = re.search(r"kResidualNodeIdExemptOpsCount\s*=\s*(\d+)", evaluator_ixx)
    m_scope = re.search(r"kResidualNodeIdScopeFilesCount\s*=\s*(\d+)", evaluator_ixx)
    m_inv = re.search(r"kResidualNodeIdInventoryCount\s*=\s*(\d+)", evaluator_ixx)
    if not (m_exempt and m_scope and m_inv):
        fails.append("AC9: residual inventory constants not defined in evaluator_security.cpp")
    else:
        if int(m_exempt.group(1)) != len(EXEMPT_2ARG_OPS):
            fails.append(
                f"AC9: kResidualNodeIdExemptOpsCount={m_exempt.group(1)} "
                f"!= EXEMPT_2ARG_OPS count={len(EXEMPT_2ARG_OPS)}"
            )
        if int(m_scope.group(1)) != len(SCOPE_FILES):
            fails.append(
                f"AC9: kResidualNodeIdScopeFilesCount={m_scope.group(1)} != SCOPE_FILES count={len(SCOPE_FILES)}"
            )
        if int(m_inv.group(1)) != len(EXEMPT_2ARG_OPS) + len(SCOPE_FILES):
            fails.append(
                f"AC9: kResidualNodeIdInventoryCount={m_inv.group(1)} "
                f"!= exempt+scope = {len(EXEMPT_2ARG_OPS) + len(SCOPE_FILES)}"
            )
    # schema-2881 must be exposed in posture prim.
    must("schema-2881", "AC9", posture)
    must("issue-2881", "AC9", posture)
    must("residual-node-id-side-effect-coverage-wired", "AC9", posture)
    must("residual-node-id-exempt-ops-count", "AC9", posture)
    must("residual-node-id-scope-files-count", "AC9", posture)
    must("residual-node-id-inventory-count", "AC9", posture)

    # ── #2881 AC10 — cross-source-cite ──
    must("Issue #2881", "AC10", sec)
    must("#2881", "AC10", posture)
    must("#2881", "AC10", compile_cpp)  # lineage preserved (was #2839 cite; #2881 in #2839 lineage)
    # Reference to #2881 must appear in the documentation block that
    # closes the residual NodeId-only inventory comment in evaluator_security.cpp.
    if "Issue #2881" not in sec:
        fails.append("AC10: evaluator_security.cpp does not cite Issue #2881")

    # ── #2881 AC11 — tests added to existing src/-aligned suite; no design ──
    # AC tests must be added to test_require_effect_auto_isolation.cpp (lineage)
    # — adding a new test_issue_2881.cpp would violate #81967.
    must("2881", "AC11", test_re)
    must("2881", "AC11", test_ts)
    if (ROOT / "tests" / "compiler" / "test_issue_2881.cpp").is_file():
        fails.append("AC11: test_issue_2881.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2881-*")):
            fails.append(f"AC11: docs/design/{f.name} present (forbidden per #1655)")

    # ── Cross-check #2689 still green ──
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
    print("OK: Issue #2839 + #2881 side-effect + fiber principal residual enforcement")
    return 0


if __name__ == "__main__":
    sys.exit(main())
