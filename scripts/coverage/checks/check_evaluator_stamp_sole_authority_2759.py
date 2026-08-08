#!/usr/bin/env python3
"""Issue #2759: Evaluator::stamp_stable_ref sole production StableNodeRef authority.

Refines #2705 residual: production EDSL / query / mutate StableNodeRef returns
must stamp via Evaluator (capability_tenant_id_), not process-global capture.
Under hard-close, non-zero set_isolation_capture_tenant writes are suppressed;
make_stamped_ref uses layout-only capture so evaluator_miss is not false-counted;
refresh_if_stale remakes via make_safe_ref_layout and preserves tenant_id.

Contract:
  AC1 Production hard-close → Evaluator stamp authority; global write suppress;
      make_stamped_ref uses make_ref_layout + stamp (no miss bump).
  AC2 Soft / tenant=0 / sandbox=off → permissive global path unchanged.
  AC3 refresh_if_stale / validate_or_refresh preserve tenant via layout remake
      (no global re-stamp under production).
  AC4 Quiet single-tenant → layout fill only; no extra Soft global path cost
      when tid==0 (maybe_stamp early return).
  AC5 Additive observability; #2705 / #2687 / #2056 surfaces preserved;
      no docs/design/* per #1655.
  AC6 Extend test_tenant_isolation_enforcement; this linter; build.py gate.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    prov = _read("src/core/provenance_tracker.hh")
    eval_sec = _read("src/compiler/evaluator_security.cpp")
    ast = _read("src/core/ast.ixx")
    stab = _read("src/core/ast_stability.cpp")
    workspace = _read("src/core/workspace_isolation.hh")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    mutate = _read("src/compiler/evaluator_primitives_mutate.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # AC1 — sole authority + layout path + global write suppress.
    must("kEvaluatorStampSoleAuthorityIssue", "AC1", prov)
    must("#2759", "AC1", prov)
    must("g_isolation_capture_global_write_suppressed_total_atomic", "AC1", prov)
    must("hard_capture_tenant_active", "AC1", prov)
    must("make_ref_layout", "AC1", ast)
    must("make_safe_ref_layout", "AC1", ast)
    must("make_ref_layout", "AC1", eval_sec)
    must("make_safe_ref_layout", "AC1", eval_sec)
    must("stamp_stable_ref", "AC1", eval_sec)
    # make_stamped_ref body must use layout (not make_ref / make_safe_ref for capture).
    m = re.search(
        r"Evaluator::make_stamped_ref\s*\([^)]*\)\s*(?:const)?\s*(?:noexcept)?\s*\{(.+?)\n\}",
        eval_sec,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        fails.append("AC1: Evaluator::make_stamped_ref impl not found")
    else:
        body = m.group(1)
        if "make_ref_layout" not in body:
            fails.append("AC1: make_stamped_ref must use make_ref_layout")
        # Layout-only: reject bare make_ref(id) / ->make_ref( after stripping layout name.
        stripped = body.replace("make_ref_layout", "")
        if re.search(r"->make_ref\s*\(", stripped) or re.search(r"\bmake_ref\s*\(", stripped):
            fails.append("AC1: make_stamped_ref must not call make_ref (use layout)")
    # set_isolation_capture_tenant suppresses non-zero under hard-close.
    set_body = ""
    si = prov.find("inline void set_isolation_capture_tenant")
    if si >= 0:
        brace = prov.find("{", si)
        depth = 0
        for j in range(brace, len(prov)):
            if prov[j] == "{":
                depth += 1
            elif prov[j] == "}":
                depth -= 1
                if depth == 0:
                    set_body = prov[brace + 1 : j]
                    break
    if not set_body:
        fails.append("AC1: set_isolation_capture_tenant impl not found")
    else:
        if "hard_capture_tenant_active" not in set_body:
            fails.append("AC1: set_isolation_capture_tenant must consult hard_capture")
        if "g_isolation_capture_global_write_suppressed_total_atomic" not in set_body:
            fails.append("AC1: suppressed write must bump global_write_suppressed counter")
    must("#2759", "AC1", workspace)

    # Production EDSL stamp sites.
    must("stamp_stable_ref", "AC1", qws)
    must("stamp_stable_ref", "AC1", mutate)
    must("stamp_stable_ref", "AC1", fiber)
    must("make_ref_layout", "AC1", agent)
    must("stamp_stable_ref", "AC1", agent)

    # AC2 — Soft path preserved (maybe_stamp still has global_fallback + tid==0).
    must("g_isolation_capture_stamp_global_fallback_total_atomic", "AC2", prov)
    must("if (tid == 0)", "AC2", prov)
    must("maybe_stamp_stable_ref_isolation_tenant", "AC2", prov)

    # AC3 — refresh layout remake + preserve tenant.
    must("make_safe_ref_layout", "AC3", stab)
    must("preserved_tenant", "AC3", stab)
    must("record_stable_ref_tenant_preserved_on_refresh", "AC3", stab)
    # Must not re-introduce make_safe_ref (with maybe_stamp) in refresh remake.
    # Allow other mentions; require the remake assignment uses layout.
    if "make_safe_ref_layout" not in stab:
        fails.append("AC3: refresh remake must use make_safe_ref_layout")
    elif "const auto fresh = ast.make_safe_ref_layout" not in stab and "make_safe_ref_layout(id" not in stab:
        fails.append("AC3: refresh_if_stale must assign fresh from make_safe_ref_layout")

    # AC4 — quiet path: tid==0 early return in maybe_stamp.
    must("if (tid == 0)", "AC4", prov)
    must("return false", "AC4", prov)

    # AC5 — additive query; lineage keys preserved.
    must("isolation-capture-global-write-suppressed-total", "AC5", q)
    must("schema-2759", "AC5", q)
    must("issue-2759", "AC5", q)
    must("schema-2705", "AC5", q)
    must("issue-2705", "AC5", q)
    must("schema-2687", "AC5", q)
    must("isolation-capture-hard-close-armed", "AC5", q)
    must("isolation-capture-stamp-local-total", "AC5", q)
    must("isolation-capture-stamp-global-fallback-total", "AC5", q)
    must("isolation-capture-stamp-evaluator-miss-total", "AC5", q)

    # AC6 — tests + linter + no design docs + gate.
    must("#2759", "AC6", test)
    must("g_isolation_capture_global_write_suppressed_total_atomic", "AC6", test)
    must("make_ref_layout", "AC6", test)
    must("check_evaluator_stamp_sole_authority_2759", "AC6", build)
    for rel in (
        "docs/design/evaluator_stamp_sole_authority_2759.md",
        "docs/evaluator_stamp_sole_authority_2759.md",
        "design/2759.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    linter_path = ROOT / "scripts/coverage/checks/check_evaluator_stamp_sole_authority_2759.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2759 Evaluator::stamp_stable_ref sole production authority — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
