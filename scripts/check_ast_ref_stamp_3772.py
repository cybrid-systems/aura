#!/usr/bin/env python3
"""Issue #3772 source-cite gate: gated re-entry for layout-only EDSL refs.

The Agent EDSL (ast:stable-ref) packs (id . gen) — the tenant stamp is
dropped at pack time. Re-entry prims (ast:ref-get / ast:ref-valid? /
ast:stable-refs-valid?) observed FlatAST layout-only with no isolation
consult, while the C++ authority (#3365) denies layout-only /
unstamped refs under Restricted+MT — a cross-tenant read/observe dual
path.

ACs:
  AC1  ast:ref-get routes through restamp_read_ref BEFORE resolve_stamped
       (no raw get_safe observe); the prim cites #3772; deny reads void.
  AC2  ast:ref-valid? and ast:stable-refs-valid? gate per-ref via
       restamp_read_ref; deny reads #f.
  AC3  the restamp_read_ref helper is declared in evaluator.ixx and
       implemented in evaluator_security.cpp citing #3772; the foreign
       deny routes through check_workspace_isolation (IsolationDeny
       face, fiber + epoch-mid joinable).
  AC4  consult regime matches the #3415 read-side shape (strict ||
       (restricted && mt)); Soft/Off keeps the legacy direct observe
       path (early allow, ref untouched).
  AC5  tests cite #3772 (runtime ACs in test_tenant_isolation_enforcement);
       no test_issue_3772.cpp; no docs/design/3772-*; build.py wires this
       linter and scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRIM = ROOT / "src" / "compiler" / "evaluator_primitives_ast.cpp"
SEC = ROOT / "src" / "compiler" / "evaluator_security.cpp"
IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
TST = ROOT / "tests" / "core" / "test_tenant_isolation_enforcement.cpp"


def _lambda_body(text: str, start_marker: str) -> str:
    pos = text.find(start_marker)
    if pos < 0:
        return ""
    end = text.find("\n    add(", pos + len(start_marker))
    if end < 0:
        end = len(text)
    return text[pos:end]


def main() -> int:
    prim = PRIM.read_text() if PRIM.exists() else ""
    sec = SEC.read_text() if SEC.exists() else ""
    ixx = IXX.read_text() if IXX.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    get_body = _lambda_body(prim, 'add("ast:ref-get",')
    valid_body = _lambda_body(prim, 'add("ast:ref-valid?",')
    bulk_pos = prim.find('"ast:stable-refs-valid?"')

    good = (
        "#3772" in get_body
        and 'restamp_read_ref(ref, "ast:ref-get")' in get_body
        and get_body.find('restamp_read_ref(ref, "ast:ref-get")') < get_body.find("ev.resolve_stamped(")
        and "workspace_flat_->get_safe" not in get_body
    )
    report("AC1", good, "ast:ref-get gated before resolve_stamped; no raw observe; cites #3772")

    good = (
        "#3772" in valid_body
        and 'restamp_read_ref(ref, "ast:ref-valid?")' in valid_body
        and "return make_bool(false);" in valid_body
        and "restamp_read_ref(sref," in prim[bulk_pos:]
        and "valid_ev = make_bool(flat.is_valid(sref));" in prim[bulk_pos:]
    )
    report("AC2", good, "ast:ref-valid? + bulk gate per-ref; deny reads #f")

    good = (
        "#3772" in sec
        and "bool Evaluator::restamp_read_ref(" in sec
        and "restamp_read_ref" in ixx
        and "check_workspace_isolation(caller, existing" in sec
    )
    report("AC3", good, "helper in security TU cites #3772; foreign deny via isolation check")

    good = (
        "const bool mt = ::aura::core::provenance::hard_capture_tenant_active()" in sec
        and "if (!(strict || (restricted && mt)))" in sec
        and "return true; // Soft/Off" in sec
    )
    report("AC4", good, "consult regime matches #3415; Soft/Off legacy path kept")

    good = (
        "#3772 AC1" in tst
        and "#3772 AC2" in tst
        and "#3772 AC4" in tst
        and "#3772 AC5" in tst
        and not (ROOT / "tests" / "core" / "test_issue_3772.cpp").exists()
        and not (ROOT / "docs" / "design" / "3772-ast-ref-stamp.md").exists()
        and "check_ast_ref_stamp_3772.py" in build
        and "check_ast_ref_stamp_3772.py" in allow
    )
    report("AC5", good, "tests cite #3772; no issue-file/doc; build.py + allowlist wired")

    print(f"check_ast_ref_stamp_3772: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
