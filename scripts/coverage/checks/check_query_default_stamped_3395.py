#!/usr/bin/env python3
# scripts/coverage/checks/check_query_default_stamped_3395.py
#
# Issue #3395: production default Agent-facing query:* must finish through
# the schema-2 QueryResult stamp path (auto-upgrade via end_query_epoch_
# maybe_result for pattern / find / by-marker; query:filter gets the same
# wrap in this ship). Under production, resolve_mutate_node_arg /
# resolve_query_node_arg must reject bare int (occupancy, not identity).
# Soft/Off keeps the historical bare list + int-stamp paths (AC3 zero-cost
# regression-free).
#
# Source-cite gate: every row asserts a concrete contract in the production
# source. Self-test builds a fake source tree in /tmp and asserts the
# --strict pass / fail behaviour. --strict is wired into build.py
# cmd_query_default_stamped_coverage.

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]  # scripts/coverage/checks/../../..
QWS_PATH = REPO_ROOT / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp"
MUT_PATH = REPO_ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
DESIGN_DIR = REPO_ROOT / "docs" / "design"
ISSUES_TESTS_DIR = REPO_ROOT / "tests" / "issues"


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def _rows() -> list[tuple[str, str, bool]]:
    """Return (label, content, ok) tuples for the source-cite contract.

    Each row is one concrete contract that must hold in the production
    source for #3395 to be considered shipped. The linter is deliberately
    file-content-scoped (not build-system / not git-history) so it stays
    stable across merges and doesn't false-positive on unrelated churn.
    """
    qws = _read(QWS_PATH)
    mut = _read(MUT_PATH)

    rows: list[tuple[str, str, bool]] = []

    # AC1: query:filter routes through the schema-2 stamp path.
    # The new `:as-query-result` opt-in keyword + auto-upgrade gate + the
    # `make_query_result_hash` wrap at the end of the retry loop must all
    # appear in query_workspace.cpp.
    rows.append(
        (
            "3395 AC1: query:filter :as-query-result opt-in keyword",
            "query:filter gains `:as-query-result` / `:query-result` opt-in "
            "(mirrors pattern/find/by-marker; default off, bare list).",
            "as_query_result = true" in qws and 'kw == ":as-query-result" || kw == ":query-result"' in qws,
        )
    )
    rows.append(
        (
            "3395 AC1: query:filter production auto-upgrade + hash wrap",
            "After the QueryEpoch retry loop, query:filter auto-upgrades under "
            "production_defaults_active and wraps via make_query_result_hash "
            "(sets reserved == kQueryResultMatchSchema2Prod).",
            "auto-upgrade → stamped hash or error" in qws and "make_query_result_hash(last_qe" in qws,
        )
    )
    # AC1: end_query_epoch_maybe_result still has the production auto-upgrade
    # for the other 3 queries (pattern/find/by-marker). Non-regress for #3286.
    rows.append(
        (
            "3395 AC1: end_query_epoch_maybe_result auto-upgrade (Issue #3286)",
            "The existing auto-upgrade inside end_query_epoch_maybe_result "
            "(used by query:pattern / find / by-marker) is unchanged.",
            "if (!as_query_result && aura::compiler::typed_audit::production_defaults_active())" in qws
            and "as_query_result = true; // auto-upgrade" in qws,
        )
    )

    # AC2: resolve_mutate_node_arg rejects bare int under production.
    rows.append(
        (
            "3395 AC2: resolve_mutate_node_arg production raw-id reject (mutate.cpp)",
            "Under production_defaults_active, the bare-int path returns "
            "stale-ref with the #3395 reject message (no make_stamped_ref, "
            "no auto-refresh — occupancy is rejected at the gate).",
            "raw node-id rejected under production" in mut and "Issue #3395" in mut,
        )
    )
    # AC2: resolve_query_node_arg mirror gate.
    rows.append(
        (
            "3395 AC2: resolve_query_node_arg production raw-id reject (query_workspace.cpp)",
            "Mirror contract on the query side: bare int under production "
            "is rejected with stale-ref / raw-id error. query:parent / "
            "query:node / query:children(-stable) all flow through this gate.",
            "raw node-id rejected under production" in qws and "Issue #3395" in qws,
        )
    )

    # AC4: non-regress for #3137 / #3311 / #3230 — the three contracts this
    # ship depends on (and does NOT regress) must still appear in source.
    rows.append(
        (
            "3395 AC4: #3137 stamp_query_result_full_provenance unchanged",
            "The schema-2 stamp helper from #3103/#3137 is still wired "
            "(make_query_result_hash calls it; stamp failure → structured "
            "restamp-lag / query-result-layout-only error).",
            "stamp_query_result_full_provenance" in qws,
        )
    )
    rows.append(
        (
            "3395 AC4: #3311 Soft→Prod reserved discriminator unchanged",
            "kQueryResultMatchSchema2Prod discriminator is still wired in the "
            "freshness validator (Issue #3311 Soft → Production arm invalidates "
            "Soft-only schema-2 results).",
            "kQueryResultMatchSchema2Prod" in qws,
        )
    )
    rows.append(
        (
            "3395 AC4: #3230 restamp-lag gate unchanged",
            "Issue #3230 torn/budget gate is still consulted before "
            "make_stamped_ref in resolve_query_node_arg / make_stamped_ref "
            "path (prevents durable QueryResult from carrying pre-mutate gen).",
            "#3230" in qws or "#3230" in mut,
        )
    )

    # AC5: no docs/design/, no tests/issues/test_issue_3395.cpp.
    rows.append(
        (
            "3395 AC5: no docs/design/3395-*.md plan doc",
            "Per the #1655 / no-design-docs directive, #3395 carries no "
            "docs/design/<issue>-<slug>.md plan doc — design rationale lives "
            "in the commit message + close comment.",
            not (DESIGN_DIR / "3395-query-default-stamped.md").exists()
            and not any(p.name.startswith("3395-") and p.suffix == ".md" for p in DESIGN_DIR.glob("3395-*.md"))
            if DESIGN_DIR.exists()
            else True,
        )
    )
    rows.append(
        (
            "3395 AC5: no tests/issues/test_issue_3395.cpp",
            "Per the agent-development test-suiting constraint, the #3395 "
            "tests live in tests/compiler/test_query_result_full_provenance.cpp "
            "(src/-aligned suite, AC5 non-regress for #3389).",
            not (ISSUES_TESTS_DIR / "test_issue_3395.cpp").exists(),
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
    print("OK: Issue #3395 query-default-stamped — all AC rows satisfied")
    return 0


def _self_test() -> int:
    """Build a fake source tree and assert --strict pass / fail behaviour.

    Pass case: write the expected patterns to fake qws.cpp + mut.cpp, no
    docs/design/3395-*, no tests/issues/test_issue_3395.cpp. All rows pass.
    Fail case: drop the production raw-id reject from mut.cpp. The AC2
    row flips to FAIL. The rest stays green.
    """

    with tempfile.TemporaryDirectory(prefix="check_query_default_stamped_3395_") as td:
        root = Path(td)
        # Mirror the production layout: scripts/coverage/checks/<this>.py
        # expects REPO_ROOT (3 levels up) to contain src/ + docs/ + tests/.
        # Build a minimal subtree and patch REPO_ROOT via env var.
        (root / "src" / "compiler").mkdir(parents=True)
        (root / "docs" / "design").mkdir(parents=True)
        (root / "tests" / "issues").mkdir(parents=True)

        qws_text = (
            "// fake qws\n"
            'kw == ":as-query-result" || kw == ":query-result"\n'
            "as_query_result = true\n"
            "// auto-upgrade → stamped hash or error\n"
            "make_query_result_hash(last_qe\n"
            "if (!as_query_result && aura::compiler::typed_audit::production_defaults_active())\n"
            "as_query_result = true; // auto-upgrade\n"
            "stamp_query_result_full_provenance\n"
            "kQueryResultMatchSchema2Prod\n"
            "raw node-id rejected under production\n"
            "Issue #3395\n"
            "#3230\n"
        )
        mut_text = "// fake mut\nraw node-id rejected under production\nIssue #3395\n"
        (root / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp").write_text(qws_text)
        (root / "src" / "compiler" / "evaluator_primitives_mutate.cpp").write_text(mut_text)

        # Patch REPO_ROOT for the duration of the self-test.
        saved = os.environ.get("AURA_3395_REPO_ROOT")
        os.environ["AURA_3395_REPO_ROOT"] = str(root)
        try:
            # The module-level REPO_ROOT is computed at import time; reload
            # this module under the patched env to pick up the new path.
            import importlib

            import check_query_default_stamped_3395 as selfmod

            importlib.reload(selfmod)
            selfmod.REPO_ROOT = Path(root)
            selfmod.QWS_PATH = Path(root / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp")
            selfmod.MUT_PATH = Path(root / "src" / "compiler" / "evaluator_primitives_mutate.cpp")
            selfmod.DESIGN_DIR = Path(root / "docs" / "design")
            selfmod.ISSUES_TESTS_DIR = Path(root / "tests" / "issues")

            print("self-test PASS case (all rows satisfied):")
            pass_rows = selfmod._rows()
            rc_pass = selfmod._run(pass_rows)
            if rc_pass != 0:
                print("self-test FAIL: pass case did not return 0")
                return 1

            # Now drop the production raw-id reject from mut.cpp → AC2 must fail.
            (root / "src" / "compiler" / "evaluator_primitives_mutate.cpp").write_text(
                "// fake mut without raw-id reject\n"
            )
            print("self-test FAIL case (mut.cpp AC2 reject missing):")
            fail_rows = selfmod._rows()
            rc_fail = selfmod._run(fail_rows)
            if rc_fail == 0:
                print("self-test FAIL: fail case did not return non-zero")
                return 1
        finally:
            if saved is None:
                os.environ.pop("AURA_3395_REPO_ROOT", None)
            else:
                os.environ["AURA_3395_REPO_ROOT"] = saved

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
