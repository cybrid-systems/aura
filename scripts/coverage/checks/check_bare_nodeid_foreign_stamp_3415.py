#!/usr/bin/env python3
"""Issue #3415: occupancy NodeId must not restamp a foreign owner as caller.

Residual of #3365 / #2942: require_effect_for_node_id always
make_stamped_ref(caller) so Restricted+MT occupancy int on a shared
workspace_flat_ passes cur==target. require_effect isolation target
was always capability_tenant_id_ (unlike resolve_stamped).

Contract:
  AC1 Restricted+MT: existing foreign stamp on NodeId → IsolationDeny
  AC2 stamped foreign on_ref still denies (#2658)
  AC3 same-tenant stamped ref still allows
  AC4 Soft / single-tenant Restricted occupancy unchanged
  AC5 no Mutate grant → deny, body zero side-effect
  AC6 extend test_require_effect_auto_isolation +
      test_tenant_isolation_enforcement; no invent / docs/design;
      no new query key
  AC7 resolve_mutate_node_arg packed path keeps stamp (no occupancy
      restamp)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    sec = _read("src/compiler/evaluator_security.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    prov = _read("src/core/provenance_tracker.hh")
    test_req = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")

    # AC1 — for_node_id consults existing stamp; require_effect target
    # uses foreign ref_tenant.
    must("Issue #3415", "AC1", sec)
    must("existing_stamp_for_node", "AC1", sec)
    must("note_stamped_node", "AC1", sec)
    must("kBareNodeIdIsolationIssue = 3415", "AC1", ixx)
    must("last_stamped_node_id", "AC1", prov)
    fn = sec.find("bool Evaluator::require_effect_for_node_id")
    fn_body = sec[fn : fn + 2800] if fn >= 0 else ""
    must("existing_stamp_for_node", "AC1 for_node_id", fn_body)
    must("require_effect_on_ref", "AC1 for_node_id", fn_body)
    must("make_stamped_ref", "AC1 for_node_id same-tenant", fn_body)
    if "make_stamped_ref(node_id)" in fn_body.split("existing_stamp_for_node")[0]:
        fails.append("AC1: for_node_id must consult existing stamp before restamping caller")

    req = sec.find("bool Evaluator::require_effect(")
    req_body = sec[req : req + 2500] if req >= 0 else ""
    must("iso_target", "AC1 require_effect", req_body)
    must("/*ref_tenant=*/ref_tenant", "AC1 require_effect", req_body)
    must("/*target=*/iso_target", "AC1 require_effect", req_body)

    must("Issue #3415", "AC1 mutate", mut)
    must("existing_stamp_for_node", "AC1 mutate occupancy", mut)

    # AC2 — on_ref foreign still the stamped path.
    must("require_effect_on_ref", "AC2", sec)
    must("Issue #2658", "AC2 lineage", sec)

    # AC3 — same-tenant still stamps caller.
    must("ref = make_stamped_ref(node_id)", "AC3", fn_body)

    # AC4 — Soft / single-tenant skip consult.
    must("restricted && mt", "AC4", fn_body)
    stamp = sec.find("void Evaluator::stamp_stable_ref")
    stamp_body = sec[stamp : stamp + 1800] if stamp >= 0 else ""
    must("sandbox_mode_ || effect_sandbox_mode() != 0", "AC4 Soft skip note", stamp_body)

    # AC5/AC6 — existing suites + no invent / no new query key.
    must("ac3415", "AC6 auto-isolation", test_req)
    must("Issue #3415", "AC6 auto-isolation", test_req)
    must("ac3415", "AC6 tenant-isolation", test_iso)
    must("Issue #3415", "AC6 tenant-isolation", test_iso)
    must("check_bare_nodeid_foreign_stamp_3415", "AC6 build", build)
    if "schema-3415" in mut or "schema-3415" in q or "schema-3415" in qws or "schema-3415" in posture:
        fails.append("AC6: new schema-3415 query key (forbidden)")
    if (ROOT / "tests" / "issues" / "test_issue_3415.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3415.cpp present (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3415.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3415.cpp present (forbidden)")
    if (ROOT / "tests" / "core" / "test_issue_3415.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3415.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3415-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    # AC7 — packed resolve keeps stamp; no occupancy restamp.
    resolve = mut.find("auto resolve_mutate_node_arg")
    packed = mut.find("if (auto packed = unpack_stable_ref_arg", resolve) if resolve >= 0 else -1
    int_br = mut.find("if (is_int(arg))", packed) if packed >= 0 else -1
    packed_body = mut[packed:int_br] if packed >= 0 and int_br > packed else ""
    must("Issue #3415", "AC7 packed", packed_body)
    if "make_stamped_ref(" in packed_body:
        fails.append("AC7: packed resolve_mutate_node_arg must not make_stamped_ref")
    must("require_effect_on_ref", "AC7 add_mutate on_ref", mut)

    if fails:
        print("check_bare_nodeid_foreign_stamp_3415: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3415 occupancy NodeId foreign-stamp isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
