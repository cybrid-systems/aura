#!/usr/bin/env python3
# scripts/coverage/checks/check_query_and_replace_guard_fail_4652.py
#
# Issue #4652: a rejected mutate:query-and-replace must fail the nested
# MutationBoundaryGuard (ok = false) and free the SoA rows the failed parse
# appended.
#
# Why a custom check (not a manifest substring row): the contract is about
# the STATEMENT SHAPE inside one primitive body:
#   1. every reject path in the mutate:query-and-replace body sets
#      ok = false BEFORE it returns an error pair, and
#   2. the three post-parse rejects (parse-failure / parent invalid /
#      child slot invalid) call the orphan free, bounded by the
#      size_before_parse snapshot.
# A file-wide "ok = false;" substring cannot tell "the reject path failed
# the Guard" from "some other path set it", so this check locates the
# mutate:query-and-replace body and inspects each return in place.
#
# Reference shape in the same file: mutate:replace-pattern already frees the
# parse appends and sets ok = false on its zero-replace abort (#2798).

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MUT_PATH = REPO_ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"

# The anchor is unique: the batch sibling is "mutate:query-and-replace-batch".
QAR_ANCHOR = '"mutate:query-and-replace",'
# The guard-acquire failure runs BEFORE the guard exists - there is nothing
# to fail, so it is the one return that may skip ok = false.
ACQUIRE_FAIL_KIND = 'return mev("resource-quota-exceeded"'


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def qar_body(text: str) -> str:
    """Return the mutate:query-and-replace primitive body."""
    start = text.find(QAR_ANCHOR)
    if start == -1:
        return ""
    # The body ends at the next add_mutate( registration.
    end = text.find("add_mutate(", start + len(QAR_ANCHOR))
    if end == -1:
        return text[start:]
    return text[start:end]


def _reject_returns(body: str):
    """(offset, return statement) for every reject return in the body."""
    out = []
    pos = 0
    while True:
        pos = body.find("return mev(", pos)
        if pos == -1:
            break
        tail = body.find(";", pos)
        if tail == -1:
            tail = pos + 40
        out.append((pos, body[pos : tail + 1]))
        pos = tail + 1
    return out


# How far back the guard assignment may sit from the reject return.
GUARD_LOOKBACK = 600


def _guards_the_return(body: str, pos: int) -> bool:
    """True when the reject return is preceded by an ok = false assignment.

    The look-back is a bounded window. Inside it the ok = false assignment
    must be the LAST guard/reject marker: an "ok = false" that belongs to an
    EARLIER reject is followed by that reject's own "return", so the last
    "return " in the window wins and the row fails.
    """
    head = body[max(0, pos - GUARD_LOOKBACK) : pos]
    ok_at = head.rfind("ok = false")
    if ok_at == -1:
        return False
    return_at = head.rfind("return ")
    return return_at == -1 or ok_at > return_at


def _rows():
    mut = _read(MUT_PATH)
    body = qar_body(mut)

    rows = []

    rows.append(
        (
            "4652 AC1: mutate:query-and-replace body is present",
            "The mutate:query-and-replace primitive must still be registered in "
            "src/compiler/evaluator_primitives_mutate.cpp (the anchor "
            '"mutate:query-and-replace", is unique).',
            bool(body),
        )
    )

    if body:
        rejects = _reject_returns(body)
        unguarded = [
            stmt for pos, stmt in rejects if not _guards_the_return(body, pos) and ACQUIRE_FAIL_KIND not in stmt
        ]
        rows.append(
            (
                "4652 AC1: every reject return in the body fails the nested Guard",
                "Each error return of mutate:query-and-replace must set ok = false "
                "in the same statement block before it returns (the guard-acquire "
                "failure runs before the guard exists and is exempt). Unguarded "
                "reject(s): " + repr(unguarded) + ".",
                bool(rejects) and not unguarded,
            )
        )
    else:
        rows.append(
            (
                "4652 AC1: every reject return in the body fails the nested Guard",
                "The mutate:query-and-replace body was not found.",
                False,
            )
        )

    # AC2: the post-parse rejects free the parse appends.
    free_helper = "free_repl_parse_orphans"
    rows.append(
        (
            "4652 AC2: the post-parse rejects free the parse appends",
            "The parse-failure / parent-invalid / child-slot-invalid rejects must "
            "call the orphan free helper (free_orphan_nodes_from bounded by "
            "size_before_parse) before they return.",
            body.count(free_helper) >= 4 and "free_orphan_nodes_from" in body and "size_before_parse" in body,
        )
    )

    for kind in (
        "parse-failure",
        "mutate:query-and-replace: parent invalid after parse",
        "mutate:query-and-replace: child slot invalid after parse",
    ):
        pos = body.find('"' + kind + '"')
        ok = pos != -1
        if ok:
            # The free call must sit between the rollback and the return.
            window = body[max(0, pos - 900) : pos]
            ok = free_helper in window
        rows.append(
            (
                "4652 AC2: the " + kind + " reject frees the parse appends",
                "The " + kind + " reject must call " + free_helper + "() before it "
                "returns (rollback_atomic_batch restores the metadata snapshot "
                "only - the appended SoA rows would stay live slots).",
                ok,
            )
        )

    # AC3: the reference shape (mutate:replace-pattern) is untouched.
    rp_anchor = '"mutate:replace-pattern"'
    rp_start = mut.find(rp_anchor)
    rp_body = ""
    if rp_start != -1:
        rp_end = mut.find("add_mutate(", rp_start + len(rp_anchor))
        rp_body = mut[rp_start : rp_end if rp_end != -1 else len(mut)]
    rows.append(
        (
            "4652 AC3: mutate:replace-pattern keeps the reference shape",
            "The in-repo reference primitive (mutate:replace-pattern) must keep "
            "freeing the parse appends and setting ok = false on its zero-replace "
            "abort.",
            bool(rp_body) and "free_orphan_nodes_from" in rp_body and "ok = false" in rp_body,
        )
    )

    # AC4: cite anchor + no new test file / design doc.
    rows.append(
        (
            "4652 AC4: the source cites the issue",
            "The mutate:query-and-replace body must cite Issue #4652 so the contract is traceable in future audits.",
            "#4652" in mut,
        )
    )
    issues_tests = REPO_ROOT / "tests" / "issues" / "test_issue_4652.cpp"
    design_dir = REPO_ROOT / "docs" / "design"
    rows.append(
        (
            "4652 AC4: no tests/issues/test_issue_4652.cpp and no design doc",
            "The #4652 rows live in the src-aligned suite "
            "(tests/compiler/test_hygiene_mutate_closed_loop.cpp) - not in "
            "tests/issues/, and the issue carries no docs/design/<issue>-*.md.",
            not issues_tests.exists()
            and not (
                design_dir.exists()
                and any(p.name.startswith("4652-") and p.suffix == ".md" for p in design_dir.glob("4652-*.md"))
            ),
        )
    )

    return rows


def _run(rows) -> int:
    failed = 0
    for label, desc, ok in rows:
        status = "PASS" if ok else "FAIL"
        print("  [" + status + "] " + label)
        if not ok:
            print("         " + desc)
            failed += 1
    print()
    if failed:
        print("FAIL: " + str(failed) + " contract row(s) failed")
        return 1
    print("OK: Issue #4652 mutate:query-and-replace guard-fail contract - all AC rows satisfied")
    return 0


def _self_test() -> int:
    """Fake-tree self test: pass case + fail case (ok = false removed)."""
    import importlib

    with tempfile.TemporaryDirectory(prefix="check_qar_guard_fail_4652_") as td:
        root = Path(td)
        (root / "src" / "compiler").mkdir(parents=True)
        (root / "docs" / "design").mkdir(parents=True)
        (root / "tests" / "issues").mkdir(parents=True)

        good = (
            "    add_mutate(\n"
            '        "mutate:query-and-replace",\n'
            "        [&](const auto& a) -> EvalValue {\n"
            "            bool ok = true;\n"
            "            if (bad) {\n"
            "                ok = false;\n"
            '                return mev("bad-arg", "x");\n'
            "            }\n"
            "            const auto size_before_parse = static_cast<std::size_t>(flat.size());\n"
            "            auto free_repl_parse_orphans = [&flat, size_before_parse]() {\n"
            "                if (size_before_parse < flat.size())\n"
            "                    (void)flat.free_orphan_nodes_from(\n"
            "                        static_cast<aura::ast::NodeId>(size_before_parse));\n"
            "            };\n"
            "            if (!repl_pr.success) {\n"
            "                flat.rollback_atomic_batch();\n"
            "                free_repl_parse_orphans();\n"
            "                ok = false;\n"
            '                return mev("parse-failure", "x");\n'
            "            }\n"
            "            if (parent_bad) {\n"
            "                flat.rollback_atomic_batch();\n"
            "                free_repl_parse_orphans();\n"
            "                ok = false;\n"
            '                return mev("stale-ref", "mutate:query-and-replace: parent invalid after parse");\n'
            "            }\n"
            "            if (slot_bad) {\n"
            "                flat.rollback_atomic_batch();\n"
            "                free_repl_parse_orphans();\n"
            "                ok = false;\n"
            '                return mev("stale-ref", "mutate:query-and-replace: child slot invalid after parse");\n'
            "            }\n"
            "            return make_bool(true);\n"
            "        });\n"
            '    add_mutate("mutate:replace-pattern", [&](const auto& a) -> EvalValue {\n'
            "        (void)flat.free_orphan_nodes_from(0);\n"
            "        ok = false;\n"
            "        return make_bool(false);\n"
            "    });\n"
            "    // Issue #4652\n"
        )
        path = root / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
        path.write_text(good)

        saved = os.environ.get("AURA_4652_REPO_ROOT")
        os.environ["AURA_4652_REPO_ROOT"] = str(root)
        try:
            import check_query_and_replace_guard_fail_4652 as selfmod

            importlib.reload(selfmod)
            selfmod.REPO_ROOT = Path(root)
            selfmod.MUT_PATH = path

            print("self-test PASS case (all rows satisfied):")
            if selfmod._run(selfmod._rows()) != 0:
                print("self-test FAIL: pass case did not return 0")
                return 1

            # Drop one ok = false -> the reject-guard row must flip to FAIL.
            path.write_text(
                good.replace(
                    '                ok = false;\n                return mev("parse-failure"',
                    '                return mev("parse-failure"',
                    1,
                )
            )
            print("self-test FAIL case (one reject no longer fails the Guard):")
            if selfmod._run(selfmod._rows()) == 0:
                print("self-test FAIL: fail case did not return non-zero")
                return 1
        finally:
            if saved is None:
                os.environ.pop("AURA_4652_REPO_ROOT", None)
            else:
                os.environ["AURA_4652_REPO_ROOT"] = saved

    print("self-test OK: pass + fail behaviour as expected")
    return 0


def main(argv) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else "")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the fake-tree self-test (pass + fail cases)",
    )
    parser.add_argument("--strict", action="store_true", help="strict mode (default)")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    return _run(_rows())


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
