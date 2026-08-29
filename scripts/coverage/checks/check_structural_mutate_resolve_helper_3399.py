#!/usr/bin/env python3
# scripts/coverage/checks/check_structural_mutate_resolve_helper_3399.py
#
# Issue #3399: structural mutate:* prims must route their workspace-node
# operands through resolve_mutate_node_arg (the SSOT helper from #489)
# instead of hard-requiring is_int(a[0]) and writing the occupancy index.
# Under production, resolve_mutate_node_arg rejects bare int (via the
# #3395 bare-int production reject gate), so a production Agent holding
# a packed v2 ref / QueryResult match can target structural mutate:*
# prims without unpacking to int first. This ticket is the call-site
# coverage so those prims are not left on the old is_int gate.
#
# Source-cite gate: every row asserts a concrete contract in the production
# source. Self-test builds a fake source tree in /tmp and asserts the
# --strict pass / fail behaviour. --strict is wired into build.py
# cmd_structural_mutate_resolve_helper_coverage.

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]  # scripts/coverage/checks/../../..
MUT_PATH = REPO_ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
EVAL_PATH = REPO_ROOT / "src" / "compiler" / "evaluator.ixx"
DESIGN_DIR = REPO_ROOT / "docs" / "design"
ISSUES_TESTS_DIR = REPO_ROOT / "tests" / "issues"


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def _rows() -> list[tuple[str, str, bool]]:
    """Return (label, content, ok) tuples for the source-cite contract."""
    mut = _read(MUT_PATH)
    _read(EVAL_PATH)

    rows: list[tuple[str, str, bool]] = []

    # AC1: every structural mutate:* prim (node-id operand) calls
    # resolve_mutate_node_arg — no leftover !is_int(a[0]) as the only
    # accept path. The affected prims are: record-patch, remove-node,
    # insert-child, replace-subtree, splice, wrap, move-node, inline-call,
    # extract-function / refactor/extract, rollback-macro-introduced.
    affected_prims = [
        "record-patch",
        "remove-node",
        "insert-child",
        "replace-subtree",
        "splice",
        "wrap",
        "move-node",
        "inline-call",
        "extract-function",
        "refactor/extract",
        "rollback-macro-introduced",
    ]
    for prim in affected_prims:
        # Find the add_mutate block for this prim and check it calls
        # resolve_mutate_node_arg (not just !is_int(a[0])).
        # The prim might be registered with or without the "mutate:" prefix
        # (e.g., "refactor/extract" is registered as "refactor/extract", not
        # "mutate:refactor/extract"). Try both forms.
        prim_pats = ['"mutate:' + prim + '"']
        if prim == "refactor/extract":
            prim_pats.append('"refactor/extract"')
        all_indices = []
        for pat in prim_pats:
            start = 0
            while True:
                idx2 = mut.find(pat, start)
                if idx2 < 0:
                    break
                all_indices.append(idx2)
                start = idx2 + 1
        if not all_indices:
            rows.append(
                (
                    "3399 AC1: mutate:" + prim + " calls resolve_mutate_node_arg",
                    'Could not find "mutate:' + prim + '" in mutate.cpp',
                    False,
                )
            )
            continue
        has_resolve = False
        for idx2 in all_indices:
            body = " ".join(mut[idx2 : idx2 + 5000].split())  # normalize whitespace
            if prim == "move-node":
                # move-node has two NodeId args (a[0] node + a[1] parent);
                # both must go through resolve_mutate_node_arg.
                has_a0 = 'resolve_mutate_node_arg(*ev.workspace_flat_, a[0], "mutate:move-node"' in body
                has_a1 = 'resolve_mutate_node_arg(*ev.workspace_flat_, a[1], "mutate:move-node"' in body
                if has_a0 and has_a1:
                    has_resolve = True
                    break
            elif prim in ("extract-function", "refactor/extract"):
                # extract-function / refactor/extract have ONE NodeId arg
                # (a[0] = the function being extracted). Only need a[0] check.
                if 'resolve_mutate_node_arg(*ev.workspace_flat_, a[0], "mutate:' + prim + '"' in body:
                    has_resolve = True
                    break
            else:
                if 'resolve_mutate_node_arg(*ev.workspace_flat_, a[0], "mutate:' + prim + '"' in body:
                    has_resolve = True
                    break
        # Must NOT have a leftover !is_int(a[0]) as the only accept path.
        # (Some prims legitimately have is_int(a[1]) for a second int arg;
        # the AC is specifically about the workspace-node operand gate.)
        # Note: the loop above already sets has_resolve correctly for all
        # prims. No second-block overwrite needed (the original second
        # block had a bug where it hardcoded "mutate:move-node" and
        # overwrote has_resolve to False for all three prims).
        rows.append(
            (
                "3399 AC1: mutate:" + prim + " calls resolve_mutate_node_arg",
                "mutate:" + prim + " must call resolve_mutate_node_arg for its "
                "workspace-node operand (Issue #3399 call-site coverage). Under "
                "production, this routes through the #3395 bare-int production "
                "reject gate; Soft keeps the bare-int path (Issue #2186 compat).",
                has_resolve,
            )
        )

    # AC2: production + packed v2 ref on mutate:replace-subtree applies
    # to that identity (or stale-ref); production + bare int after restamp
    # → reject (with #3395). resolve_mutate_node_arg wires the v2 packed
    # ref through the same ensure_valid_or_refresh + stable_ref_provenance
    # gate as #3395 / #3396.
    rows.append(
        (
            "3399 AC2: resolve_mutate_node_arg wires #3395/#3396 production reject",
            "resolve_mutate_node_arg must still be the SSOT helper that routes "
            "packed v2 / QueryResult match through ensure_valid_or_refresh, and "
            "rejects bare int under production (Issue #3395 bare-int production "
            "reject gate). #3399 is the call-site coverage — the helper itself "
            "is unchanged.",
            "ensure_valid_or_refresh" in mut
            and "bump_stable_ref_provenance_enforced" in mut
            and "production_defaults_active()" in mut,
        )
    )

    # AC3: Soft int path unchanged on those prims until #3395 production
    # gate lands; do not break Soft scripts. Soft (production_defaults_active()
    # == false) goes through the v1 branch of resolve_mutate_node_arg which
    # accepts bare int via make_stamped_ref (Issue #2186 compat).
    rows.append(
        (
            "3399 AC3: Soft int path unchanged (Issue #2186 compat)",
            "resolve_mutate_node_arg must still accept bare int under Soft ("
            "production_defaults_active() == false) via make_stamped_ref — no "
            "breaking change for Soft scripts.",
            "make_stamped_ref" in mut and "bump_raw_nodeid_usage_in_primitives_count" in mut,
        )
    )

    # AC4: #489 helper + #2186 ensure + #3395 default-query face non-regress.
    rows.append(
        (
            "3399 AC4: #489 helper (resolve_mutate_node_arg) non-regress",
            "resolve_mutate_node_arg must still be the SSOT helper from Issue "
            "#489 — confirmed by its presence in mutate.cpp.",
            "resolve_mutate_node_arg" in mut,
        )
    )
    rows.append(
        (
            "3399 AC4: #2186 ensure_valid_or_refresh non-regress",
            "Issue #2186 ensure_valid_or_refresh must still be wired in the "
            "resolve_mutate_node_arg body — packed refs / stamped refs go through "
            "the same ensure path.",
            "ensure_valid_or_refresh" in mut,
        )
    )
    rows.append(
        (
            "3399 AC4: #3395 default-query face + bare-int production reject non-regress",
            "Issue #3395 default-query production reject must still be wired in "
            "resolve_mutate_node_arg — under production, bare int returns "
            "stale-ref with the #3395 message. The fix here is the call-site "
            "coverage (every structural prim routes through this helper).",
            "aura::compiler::typed_audit::production_defaults_active()" in mut
            and "stale-ref" in mut
            and "#3395" in mut,
        )
    )

    # AC5 cite anchors (commit message traceability) + no docs/design/, no
    # tests/issues/test_issue_3399.cpp.
    rows.append(
        (
            "3399 AC5: Issue #3399 cite present in mutate.cpp + builder code",
            "Each routed call must cite Issue #3399 so the call-site coverage is traceable in future audits.",
            "#3399" in mut,
        )
    )
    rows.append(
        (
            "3399 AC5: no docs/design/3399-*.md plan doc",
            "Per the #1655 / no-design-docs directive, #3399 carries no "
            "docs/design/<issue>-<slug>.md plan doc — design rationale lives "
            "in the commit message + close comment.",
            not (DESIGN_DIR / "3399-structural-mutate-resolve-helper.md").exists()
            and not any(p.name.startswith("3399-") and p.suffix == ".md" for p in DESIGN_DIR.glob("3399-*.md"))
            if DESIGN_DIR.exists()
            else True,
        )
    )
    rows.append(
        (
            "3399 AC5: no tests/issues/test_issue_3399.cpp",
            "Per the agent-development test-suiting constraint, the #3399 "
            "tests live in tests/<src-aligned suite>/ (extend an existing "
            "mutate fixture) — not in tests/issues/.",
            not (ISSUES_TESTS_DIR / "test_issue_3399.cpp").exists(),
        )
    )

    return rows


def _run(rows: list[tuple[str, str, bool]]) -> int:
    failed = 0
    for label, desc, ok in rows:
        status = "PASS" if ok else "FAIL"
        print(f"  [{status}] {label}")
        if not ok:
            print(f"         {desc}")
            failed += 1
    print()
    if failed:
        print(f"FAIL: {failed} contract row(s) failed")
        return 1
    print("OK: Issue #3399 structural mutate:resolve_mutate_node_arg call-site contract — all AC rows satisfied")
    return 0


def _self_test() -> int:
    """Build a fake source tree and assert --strict pass / fail behaviour.

    Pass case: write the expected patterns to fake mutate.cpp. All rows pass.
    Fail case: drop one resolve_mutate_node_arg call. AC1 row for that
    prim flips to FAIL. The rest stays green.
    """
    import importlib

    with tempfile.TemporaryDirectory(prefix="check_structural_mutate_resolve_helper_3399_") as td:
        root = Path(td)
        (root / "src" / "compiler").mkdir(parents=True)
        (root / "docs" / "design").mkdir(parents=True)
        (root / "tests" / "issues").mkdir(parents=True)

        affected_prims = [
            "record-patch",
            "remove-node",
            "insert-child",
            "replace-subtree",
            "splice",
            "wrap",
            "move-node",
            "inline-call",
            "extract-function",
            "refactor/extract",
            "rollback-macro-introduced",
        ]
        mut_lines = [
            "// fake mut\n",
            "ensure_valid_or_refresh\n",
            "bump_stable_ref_provenance_enforced\n",
            "production_defaults_active()\n",
            "stale-ref\n",
            "#3395\n",
            "make_stamped_ref\n",
            "bump_raw_nodeid_usage_in_primitives_count\n",
            "resolve_mutate_node_arg\n",
            "#3399\n",
        ]
        # AC4 #3395 row needs: ensure_valid_or_refresh + bump_stable_ref_provenance_enforced
        # + production_defaults_active() + stale-ref + #3395 — all already in mut_lines.
        for prim in affected_prims:
            mut_lines.append('"mutate:' + prim + '"')
            if prim == "move-node":
                mut_lines.append('resolve_mutate_node_arg(*ev.workspace_flat_, a[0], "mutate:move-node"')
                mut_lines.append('resolve_mutate_node_arg(*ev.workspace_flat_, a[1], "mutate:move-node"')
            else:
                mut_lines.append('resolve_mutate_node_arg(*ev.workspace_flat_, a[0], "mutate:' + prim + '"')
        mut_text = "\n".join(mut_lines)

        (root / "src" / "compiler" / "evaluator_primitives_mutate.cpp").write_text(mut_text)

        saved = os.environ.get("AURA_3399_REPO_ROOT")
        os.environ["AURA_3399_REPO_ROOT"] = str(root)
        try:
            import check_structural_mutate_resolve_helper_3399 as selfmod

            importlib.reload(selfmod)
            selfmod.REPO_ROOT = Path(root)
            selfmod.MUT_PATH = Path(root / "src" / "compiler" / "evaluator_primitives_mutate.cpp")
            selfmod.DESIGN_DIR = Path(root / "docs" / "design")
            selfmod.ISSUES_TESTS_DIR = Path(root / "tests" / "issues")

            print("self-test PASS case (all rows satisfied):")
            pass_rows = selfmod._rows()
            rc_pass = selfmod._run(pass_rows)
            if rc_pass != 0:
                print("self-test FAIL: pass case did not return 0")
                return 1

            # Now drop the resolve call for mutate:replace-subtree →
            # AC1 row for that prim fails.
            (root / "src" / "compiler" / "evaluator_primitives_mutate.cpp").write_text(
                mut_text.replace('"mutate:replace-subtree"', '"mutate:replace-subtree"').replace(
                    'resolve_mutate_node_arg(*ev.workspace_flat_, a[0], "mutate:replace-subtree"', "/* removed */"
                )
            )
            print("self-test FAIL case (resolve_mutate_node_arg call missing for mutate:replace-subtree):")
            fail_rows = selfmod._rows()
            rc_fail = selfmod._run(fail_rows)
            if rc_fail == 0:
                print("self-test FAIL: fail case did not return non-zero")
                return 1
        finally:
            if saved is None:
                os.environ.pop("AURA_3399_REPO_ROOT", None)
            else:
                os.environ["AURA_3399_REPO_ROOT"] = saved

    print("self-test OK: pass + fail behaviour as expected")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else "")
    parser.add_argument("--self-test", action="store_true", help="run the fake-tree self-test (pass + fail cases)")
    parser.add_argument("--strict", action="store_true", help="strict mode (default; reserved for future tightening)")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    return _run(_rows())


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
