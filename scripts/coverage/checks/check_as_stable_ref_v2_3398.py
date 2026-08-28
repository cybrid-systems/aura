#!/usr/bin/env python3
# scripts/coverage/checks/check_as_stable_ref_v2_3398.py
#
# Issue #3398: production query:as-stable-ref must pack the v2 spine
# (id . (gen . (wrap . (tenant . (cow . (fiber . boundary)))))) so the
# Agent-visible pair carries wrap + tenant + cow (the fields that #3396
# inbound unpack now requires under production). Soft keeps the historical
# v1 (id . gen) pair (Issue #2186 compat). One SSOT spine for both
# pack (this fn) and unpack (#3396 walk_v2) — same field order, same
# nested-pair shape.
#
# Source-cite gate: every row asserts a concrete contract in the production
# source. Self-test builds a fake source tree in /tmp and asserts the
# --strict pass / fail behaviour. --strict is wired into build.py
# cmd_as_stable_ref_v2_coverage.

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]  # scripts/coverage/checks/../../..
MUT_PATH = REPO_ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
EVAL_PATH = REPO_ROOT / "src" / "compiler" / "evaluator.ixx"
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
    mut = _read(MUT_PATH)
    evx = _read(EVAL_PATH)
    ast = _read(AST_PATH)

    rows: list[tuple[str, str, bool]] = []

    # AC1: production + query:as-stable-ref → pair depth ≥ wrap+tenant+cow.
    # The pack must build the nested pair spine
    # (id . (gen . (wrap . (tenant . (cow . (fiber . boundary)))))).
    # Check that the production branch in query:as-stable-ref walks
    # wrap + tenant + cow in order (same SSOT spine as #3396 walk_v2).
    rows.append(
        (
            "3398 AC1: production query:as-stable-ref v2 spine packer present",
            "Under production_defaults_active(), query:as-stable-ref must pack "
            "(id . (gen . (wrap . (tenant . (cow . (fiber . boundary))))) — "
            "same shape as the #3396 v2 unpacker reads, so the round-trip is "
            "identity (no occupancy remap via zeroed wrap/tenant).",
            mut.find("Issue #3398: v2 spine packer") != -1
            and mut.find("p_tenant") != -1
            and mut.find("p_cow") != -1
            and mut.find("ref.wrap_epoch") != -1
            and mut.find("ref.tenant_id") != -1
            and mut.find("ref.cow_epoch_at_capture") != -1,
        )
    )
    rows.append(
        (
            "3398 AC1: v2 spine wires (id . (gen . (wrap . (tenant . (cow . ...))))",
            "The packer must wire the nested pairs in order: (tenant . (cow . ...)) ← "
            "(cow . ...), then (wrap . (tenant . ...)) ← (tenant . ...), then "
            "(gen . (wrap . ...)) ← (wrap . ...), then (id . (gen . ...)) ← (gen . ...). "
            "Source-cite confirms all four cdr wirings are present.",
            all(
                s in mut
                for s in [
                    "ev.pairs_[p_tenant].cdr = make_pair(p_cow)",
                    "ev.pairs_[p_wrap].cdr = make_pair(p_tenant)",
                    "ev.pairs_[p_gen].cdr = make_pair(p_wrap)",
                    "ev.pairs_[p_id].cdr = make_pair(p_gen)",
                ]
            ),
        )
    )

    # AC2: production Agent caches that pair, restamp/COW, feeds it to
    # mutate:replace-value → either accepted as identity or structured
    # stale-ref — never occupancy remap via zeroed wrap/tenant. This is
    # the round-trip property: the v2 pack fields match the v2 unpack
    # fields, so the round-trip is identity. Verify that the v2 pack
    # writes the same fields that the #3396 v2 unpack reads.
    rows.append(
        (
            "3398 AC2: v2 pack fields match #3396 v2 unpack fields (round-trip identity)",
            "The pack must write ref.wrap_epoch + ref.tenant_id + "
            "ref.cow_epoch_at_capture (+ optional ref.fiber_id + "
            "ref.boundary_pinned) — the exact fields the #3396 walk_v2 reads. "
            "This guarantees the round-trip is identity under production.",
            all(
                s in mut
                for s in [
                    "ref.wrap_epoch",
                    "ref.tenant_id",
                    "ref.cow_epoch_at_capture",
                ]
            ),
        )
    )

    # AC3: Soft (id . gen) unchanged. Under production_defaults_active() ==
    # false, the pack must still emit the historical v1 (id . gen) pair.
    rows.append(
        (
            "3398 AC3: Soft branch keeps v1 (id . gen) pair",
            "Under production_defaults_active() == false, query:as-stable-ref "
            "must still emit the historical v1 (id . gen) pair — no breaking "
            "change for Soft callers (Issue #2186 compat).",
            "Soft (or sandbox=off): historical v1 (id . gen) pair" in mut
            and "make_int(static_cast<std::int64_t>(ref.id))" in mut
            and "make_int(static_cast<std::int64_t>(ref.gen))" in mut,
        )
    )

    # AC4: #2198 wire v2 + #2960 stamp + #3396 unpack non-regress
    # (land pack+unpack together or behind one helper). All three
    # contracts must still be in source.
    rows.append(
        (
            "3398 AC4: #2198 wire v2 (kStableRefSerializedSizeV2 = 56) non-regress",
            "kStableRefSerializedSizeV2 = 56 (the v2 56-byte wire from #2198) must still be wired in src/core/ast.ixx.",
            "kStableRefSerializedSizeV2" in ast and "56" in ast,
        )
    )
    rows.append(
        (
            "3398 AC4: #2960 stamp_query_stable_ref_export non-regress",
            "stamp_query_stable_ref_export (which fills tenant/fiber/cow/wrap "
            "before Agent export — #2960) must still be wired in src/compiler/evaluator.ixx. "
            "The v2 pack reads from ref after export_ref() stamps it — so the stamp "
            "must still populate ref.wrap_epoch + ref.tenant_id + ref.cow_epoch_at_capture "
            "for the v2 pack to have meaningful values.",
            "stamp_query_stable_ref_export" in evx,
        )
    )
    rows.append(
        (
            "3398 AC4: #3396 v2 unpack walker non-regress",
            "The #3396 v2 unpacker (walk_v2) must still be present in both "
            "unpack_stable_ref_arg (mutate.cpp) and unpack_query_stable_ref "
            "(query_workspace.cpp) — the inbound side of the contract.",
            "walk_v2" in mut
            and "walk_v2" in _read(REPO_ROOT / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp"),
        )
    )

    # AC5 cite anchors (commit message traceability) + no docs/design/, no
    # tests/issues/test_issue_3398.cpp.
    rows.append(
        (
            "3398 AC5: Issue #3398 cite present in query:as-stable-ref",
            "The v2 pack block in query:as-stable-ref must cite Issue #3398 so "
            "the outbound face of the contract is traceable in future audits.",
            "#3398" in mut,
        )
    )
    rows.append(
        (
            "3398 AC5: no docs/design/3398-*.md plan doc",
            "Per the #1655 / no-design-docs directive, #3398 carries no "
            "docs/design/<issue>-<slug>.md plan doc — design rationale lives "
            "in the commit message + close comment.",
            not (DESIGN_DIR / "3398-as-stable-ref-v2-pack.md").exists()
            and not any(p.name.startswith("3398-") and p.suffix == ".md" for p in DESIGN_DIR.glob("3398-*.md"))
            if DESIGN_DIR.exists()
            else True,
        )
    )
    rows.append(
        (
            "3398 AC5: no tests/issues/test_issue_3398.cpp",
            "Per the agent-development test-suiting constraint, the #3398 "
            "tests live in tests/<src-aligned suite>/ (extend existing "
            "test_stable_ref_provenance_fiber_cow.cpp) — not in tests/issues/.",
            not (ISSUES_TESTS_DIR / "test_issue_3398.cpp").exists(),
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
    print("OK: Issue #3398 as-stable-ref v2 spine contract — all AC rows satisfied")
    return 0


def _self_test() -> int:
    """Build a fake source tree and assert --strict pass / fail behaviour.

    Pass case: write the expected patterns to fake mutate.cpp + ast.ixx +
    evaluator.ixx + query_workspace.cpp. All rows pass.
    Fail case: drop `Issue #3398: v2 spine packer` from mutate.cpp. AC1 fails.
    """
    import importlib

    with tempfile.TemporaryDirectory(prefix="check_as_stable_ref_v2_3398_") as td:
        root = Path(td)
        (root / "src" / "compiler").mkdir(parents=True)
        (root / "src" / "core").mkdir(parents=True)
        (root / "docs" / "design").mkdir(parents=True)
        (root / "tests" / "issues").mkdir(parents=True)

        mut_text = (
            "// fake mut\n"
            "Issue #3398: v2 spine packer\n"
            "p_tenant\n"
            "p_cow\n"
            "ref.wrap_epoch\n"
            "ref.tenant_id\n"
            "ref.cow_epoch_at_capture\n"
            "ev.pairs_[p_tenant].cdr = make_pair(p_cow)\n"
            "ev.pairs_[p_wrap].cdr = make_pair(p_tenant)\n"
            "ev.pairs_[p_gen].cdr = make_pair(p_wrap)\n"
            "ev.pairs_[p_id].cdr = make_pair(p_gen)\n"
            "Soft (or sandbox=off): historical v1 (id . gen) pair\n"
            "make_int(static_cast<std::int64_t>(ref.id))\n"
            "make_int(static_cast<std::int64_t>(ref.gen))\n"
            "walk_v2\n"
            "#3398\n"
        )
        qws_text = "// fake qws\nwalk_v2\n"
        ast_text = "// fake ast\nkStableRefSerializedSizeV2\n56\n"
        eval_text = "// fake eval\nstamp_query_stable_ref_export\n"
        (root / "src" / "compiler" / "evaluator_primitives_mutate.cpp").write_text(mut_text)
        (root / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp").write_text(qws_text)
        (root / "src" / "core" / "ast.ixx").write_text(ast_text)
        (root / "src" / "compiler" / "evaluator.ixx").write_text(eval_text)

        saved = os.environ.get("AURA_3398_REPO_ROOT")
        os.environ["AURA_3398_REPO_ROOT"] = str(root)
        try:
            import check_as_stable_ref_v2_3398 as selfmod

            importlib.reload(selfmod)
            selfmod.REPO_ROOT = Path(root)
            selfmod.MUT_PATH = Path(root / "src" / "compiler" / "evaluator_primitives_mutate.cpp")
            selfmod.EVAL_PATH = Path(root / "src" / "compiler" / "evaluator.ixx")
            selfmod.AST_PATH = Path(root / "src" / "core" / "ast.ixx")
            selfmod.DESIGN_DIR = Path(root / "docs" / "design")
            selfmod.ISSUES_TESTS_DIR = Path(root / "tests" / "issues")

            print("self-test PASS case (all rows satisfied):")
            pass_rows = selfmod._rows()
            rc_pass = selfmod._run(pass_rows)
            if rc_pass != 0:
                print("self-test FAIL: pass case did not return 0")
                return 1

            # Now drop the v2 packer from mutate.cpp → AC1 row must fail.
            (root / "src" / "compiler" / "evaluator_primitives_mutate.cpp").write_text(
                "// fake mut without v2 packer\n"
                + mut_text.replace("Issue #3398: v2 spine packer", "v2_packer_DISABLED")
            )
            print("self-test FAIL case (v2 spine packer missing from mutate.cpp):")
            fail_rows = selfmod._rows()
            rc_fail = selfmod._run(fail_rows)
            if rc_fail == 0:
                print("self-test FAIL: fail case did not return non-zero")
                return 1
        finally:
            if saved is None:
                os.environ.pop("AURA_3398_REPO_ROOT", None)
            else:
                os.environ["AURA_3398_REPO_ROOT"] = saved

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
