#!/usr/bin/env python3
# scripts/coverage/checks/check_query_children_stable_no_tls_3397.py
#
# Issue #3397: production Agent query export must not return
# children_stable_span_view (TLS pin valid only until next same-thread call).
# An Agent that treats children_stable_span_view as multi-round memory holds
# a dangling pin across Guard (UAF or wrong-generation child ids on read).
#
# Source-cite gate: every row asserts a concrete contract in the production
# source. The current query:children-stable EDSL primitive uses
# pin_query_children (SafePCVSpan, not the TLS span view) + the
# pcv_span_for_agent_export fingerprint gate (#3328) +
# force_refresh_pcv_span fallback (#3167) + structured stale-span error
# (#3397 AC2). Self-test builds a fake source tree in /tmp and asserts the
# --strict pass / fail behaviour. --strict is wired into build.py
# cmd_query_children_stable_no_tls_coverage.

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]  # scripts/coverage/checks/../../..
QWS_PATH = REPO_ROOT / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp"
AST_PATH = REPO_ROOT / "src" / "core" / "ast.ixx"
DESIGN_DIR = REPO_ROOT / "docs" / "design"
ISSUES_TESTS_DIR = REPO_ROOT / "tests" / "issues"


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def _rows() -> list[tuple[str, str, bool]]:
    """Return (label, content, ok) tuples for the source-cite contract."""
    qws = _read(QWS_PATH)
    ast = _read(AST_PATH)

    rows: list[tuple[str, str, bool]] = []

    # AC1: query:children-stable Agent return path must not use the TLS
    # span view (children_stable_span_view). The current primitive uses
    # pin_query_children (SafePCVSpan) + pcv_span_for_agent_export.
    rows.append(
        (
            "3397 AC1: query:children-stable does NOT call children_stable_span_view (TLS pin)",
            "The Agent-facing return path for (query :children-stable ...) must "
            "use pin_query_children (SafePCVSpan) + pcv_span_for_agent_export, "
            "NOT the TLS buffer-returning children_stable_span_view. Comments "
            "may mention the string for contract documentation; only function "
            "calls (children_stable_span_view(...)) are forbidden in qws.cpp.",
            not bool(re.findall(r"children_stable_span_view\s*\(", qws)),
        )
    )
    rows.append(
        (
            "3397 AC1: query:children-stable uses pin_query_children (SafePCVSpan)",
            "The Agent-facing return path must route through pin_query_children "
            "so the SafePCVSpan + pcv_span_for_agent_export fingerprint gate "
            "covers the read.",
            "pin_query_children" in qws and "pcv_span_for_agent_export" in qws,
        )
    )

    # AC2: span held across Guard under production surfaces stale-span
    # (NOT a green child list). The current primitive returns stale-span
    # after force_refresh_pcv_span fails to recover.
    rows.append(
        (
            "3397 AC2: query:children-stable returns stale-span on unrefreshable fingerprint",
            "After force_refresh_pcv_span fails (owner gone / unrefreshable), "
            "the primitive must surface structured stale-span (reuses #3167 "
            "fingerprint) — never a green pre-mutate child list.",
            'mev("stale-span"' in qws and "force_refresh_pcv_span" in qws,
        )
    )
    rows.append(
        (
            "3397 AC2: pcv_span_for_agent_export fingerprint gate wired",
            "Issue #3328: production + kids.is_stale(...) → force_refresh_pcv_span "
            "with structured stale-span on recovery failure. Source-cite confirms "
            "the gate exists in query:children-stable.",
            "pcv_span_for_agent_export" in qws and "is_stale(" in qws,
        )
    )

    # AC3: Soft / unit zero extra beyond existing pin metrics. The
    # pcv_span_for_agent_export helper is identity under Soft (zero-cost).
    rows.append(
        (
            "3397 AC3: Soft / unit zero extra beyond existing pin metrics",
            "pcv_span_for_agent_export takes `bool production` as a parameter "
            "and short-circuits to the frozen view under !production (zero "
            "extra allocs, no extra atomics). Caller passes "
            "typed_audit::production_defaults_active() — the gate is in the "
            "caller, not ast.ixx itself.",
            "pcv_span_for_agent_export" in ast
            and (
                "!production" in ast
                or "if (!production)" in ast
                or "frozen view" in ast.lower()
                or "zero extra" in ast.lower()
            ),
        )
    )

    # AC4: #3167 fingerprint / #3393 force-exclusive / #3328 refresh
    # non-regress. All three contracts must still be in source.
    rows.append(
        (
            "3397 AC4: #3167 fingerprint + stale-across-guard counter unchanged",
            "Issue #3167 pcv_span_stale_across_guard_total counter + "
            "force_refresh_pcv_span auto-bump must still be wired (non-regress "
            "for the AC2 stale-span path).",
            "pcv_span_stale_across_guard_total" in ast and "force_refresh_pcv_span" in ast and "#3167" in ast,
        )
    )
    rows.append(
        (
            "3397 AC4: #3328 production re-use face unchanged",
            "Issue #3328 fingerprint mismatch → force_refresh_pcv_span path "
            "in pcv_span_for_agent_export must still be the production "
            "Agent-export / re-use gate.",
            "#3328" in ast and "pcv_span_for_agent_export" in ast,
        )
    )
    rows.append(
        (
            "3397 AC4: #3397 cite present in query:children-stable / pcv_span_for_agent_export",
            "Commit message anchor — both query_workspace.cpp and ast.ixx must "
            "cite Issue #3397 so the no-TLS-span contract is traceable.",
            "#3397" in qws and "#3397" in ast,
        )
    )

    # AC5: no docs/design/, no tests/issues/test_issue_3397.cpp.
    rows.append(
        (
            "3397 AC5: no docs/design/3397-*.md plan doc",
            "Per the #1655 / no-design-docs directive, #3397 carries no "
            "docs/design/<issue>-<slug>.md plan doc — design rationale lives "
            "in the commit message + close comment.",
            not (DESIGN_DIR / "3397-children-stable-no-tls-span.md").exists()
            and not any(p.name.startswith("3397-") and p.suffix == ".md" for p in DESIGN_DIR.glob("3397-*.md"))
            if DESIGN_DIR.exists()
            else True,
        )
    )
    rows.append(
        (
            "3397 AC5: no tests/issues/test_issue_3397.cpp",
            "Per the agent-development test-suiting constraint, the #3397 "
            "tests live in tests/compiler/<src-aligned suite>/ (extend "
            "test_pcv_children_safe_default_migration.cpp) — not in "
            "tests/issues/.",
            not (ISSUES_TESTS_DIR / "test_issue_3397.cpp").exists(),
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
    print("OK: Issue #3397 query:children-stable no-TLS-span — all AC rows satisfied")
    return 0


def _self_test() -> int:
    """Build a fake source tree and assert --strict pass / fail behaviour.

    Pass case: write the expected patterns to fake qws.cpp + ast.ixx, no
    docs/design/3397-*, no tests/issues/test_issue_3397.cpp. All rows pass.
    Fail case: introduce 'children_stable_span_view' in the fake qws.cpp.
    The AC1 row flips to FAIL. The rest stays green.
    """
    import importlib

    with tempfile.TemporaryDirectory(prefix="check_query_children_stable_no_tls_3397_") as td:
        root = Path(td)
        # Mirror the production layout: scripts/coverage/checks/<this>.py
        # expects REPO_ROOT (3 levels up) to contain src/ + docs/ + tests/.
        (root / "src" / "compiler").mkdir(parents=True)
        (root / "src" / "core").mkdir(parents=True)
        (root / "docs" / "design").mkdir(parents=True)
        (root / "tests" / "issues").mkdir(parents=True)

        qws_text = (
            "// fake qws\n"
            "pin_query_children\n"
            "pcv_span_for_agent_export\n"
            'mev("stale-span"\n'
            "force_refresh_pcv_span\n"
            "is_stale(\n"
            "#3397\n"
        )
        ast_text = (
            "// fake ast\n"
            "pcv_span_for_agent_export\n"
            "if (!production) return safe; // Soft short-circuit, frozen view, zero extra\n"
            "pcv_span_stale_across_guard_total\n"
            "force_refresh_pcv_span\n"
            "#3167\n"
            "#3328\n"
            "#3397\n"
        )
        (root / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp").write_text(qws_text)
        (root / "src" / "core" / "ast.ixx").write_text(ast_text)

        # Patch REPO_ROOT for the duration of the self-test.
        saved = os.environ.get("AURA_3397_REPO_ROOT")
        os.environ["AURA_3397_REPO_ROOT"] = str(root)
        try:
            import check_query_children_stable_no_tls_3397 as selfmod

            importlib.reload(selfmod)
            selfmod.REPO_ROOT = Path(root)
            selfmod.QWS_PATH = Path(root / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp")
            selfmod.AST_PATH = Path(root / "src" / "core" / "ast.ixx")
            selfmod.DESIGN_DIR = Path(root / "docs" / "design")
            selfmod.ISSUES_TESTS_DIR = Path(root / "tests" / "issues")

            print("self-test PASS case (all rows satisfied):")
            pass_rows = selfmod._rows()
            rc_pass = selfmod._run(pass_rows)
            if rc_pass != 0:
                print("self-test FAIL: pass case did not return 0")
                return 1

            # Now introduce 'children_stable_span_view(...)' as an actual
            # function call in the fake qws.cpp → AC1 row must fail.
            (root / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp").write_text(
                "// fake qws with TLS span view leak\nauto kids = children_stable_span_view(node);\n" + qws_text
            )
            print("self-test FAIL case (children_stable_span_view leaked into qws):")
            fail_rows = selfmod._rows()
            rc_fail = selfmod._run(fail_rows)
            if rc_fail == 0:
                print("self-test FAIL: fail case did not return non-zero")
                return 1
        finally:
            if saved is None:
                os.environ.pop("AURA_3397_REPO_ROOT", None)
            else:
                os.environ["AURA_3397_REPO_ROOT"] = saved

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
