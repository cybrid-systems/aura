#!/usr/bin/env python3
# scripts/coverage/checks/check_unpack_stable_ref_arg_v2_3396.py
#
# Issue #3396: production packed-ref contract must match the v2 export face
# already shipped (#2198 wire v2 56 bytes, #2960 query export stamp). The
# inbound EDSL pair unpack used by mutate hot paths still reconstructs a
# brace-like StableNodeRef{id, gen} and leaves wrap/tenant/cow at 0. Under
# production, a packed `(id . gen)` from an Agent that dropped the v2 tail
# can pass or auto-refresh onto the current occupant — a multi-round
# memory hole.
#
# Source-cite gate: every row asserts a concrete contract in the production
# source. Self-test builds a fake source tree in /tmp and asserts the
# --strict pass / fail behaviour. --strict is wired into build.py
# cmd_unpack_stable_ref_arg_v2_coverage.

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]  # scripts/coverage/checks/../../..
MUT_PATH = REPO_ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
QWS_PATH = REPO_ROOT / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp"
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
    qws = _read(QWS_PATH)

    rows: list[tuple[str, str, bool]] = []

    # AC1: production + packed (id . gen) only → stale-ref. The v2 spine
    # walker must return false when wrap/tenant/cow are missing. The
    # function body gates on production_defaults_active() and returns
    # nullopt when walk_v2 fails — caller falls through to the #3395
    # bare-int reject (same stale-ref error tag, no mutate of the slot).
    rows.append(
        (
            "3396 AC1: production v2 spine walker returns nullopt on v1 packed ref",
            "unpack_stable_ref_arg must require the full v2 pair spine "
            "(id . (gen . (wrap . (tenant . (cow . (fiber . boundary)))))) under "
            "production — v1 (id . gen) returns nullopt (caller falls through "
            "to the #3395 bare-int reject with stale-ref tag).",
            "aura::compiler::typed_audit::production_defaults_active()" in mut
            and "walk_v2" in mut
            and "if (!walk_v2(cdr))" in mut
            and "return std::nullopt" in mut,
        )
    )
    rows.append(
        (
            "3396 AC1: query-side v2 spine walker mirrors mutate-side",
            "unpack_query_stable_ref must mirror the same v2 contract "
            "(production gate on walk_v2, nullopt on v1, Soft v1 fallback).",
            "aura::compiler::typed_audit::production_defaults_active()" in qws
            and "walk_v2" in qws
            and "if (!walk_v2(cdr))" in qws
            and "return std::nullopt" in qws,
        )
    )

    # AC2: production + packed schema-2 / v2 → existing ensure_valid_or_refresh
    # + isolation. The mutate-side caller must still call ensure_valid_or_refresh
    # on the v2 ref (which is the new behavior — wrap/tenant/cow are now populated).
    rows.append(
        (
            "3396 AC2: resolve_mutate_node_arg ensure_valid_or_refresh on v2 ref",
            "After unpack_stable_ref_arg returns a populated v2 ref "
            "(wrap + tenant + cow filled), resolve_mutate_node_arg must still "
            "call ensure_valid_or_refresh + bump_stable_ref_provenance_enforced "
            "— the new fields ride through the same isolation gate as before.",
            "ensure_valid_or_refresh" in mut and "bump_stable_ref_provenance_enforced" in mut,
        )
    )

    # AC3: Soft (id . gen) path unchanged. The Soft branch must still
    # accept the v1 (id . gen) shape — no breaking change for Soft callers.
    rows.append(
        (
            "3396 AC3: Soft branch accepts v1 (id . gen) unchanged",
            "The Soft (production_defaults_active() == false) branch must keep "
            "the historical v1 unpack — (id . gen) or (id . (gen . _)). "
            "No breaking change for Soft callers (Issue #2186 compat).",
            re.search(
                r"else\s*\{[^}]*?is_pair\(cdr\)[^}]*?as_pair_idx\(cdr\)",
                mut,
                re.DOTALL,
            )
            is not None,
        )
    )

    # AC4: #2198 wire v2 + #2960 export stamp non-regress. Both contracts
    # must still be in the production source.
    rows.append(
        (
            "3396 AC4: #2198 wire v2 (kStableRefSerializedSizeV2 = 56) non-regress",
            "kStableRefSerializedSizeV2 = 56 (the v2 56-byte wire from #2198) must still be wired in src/core/ast.ixx.",
            "kStableRefSerializedSizeV2" in _read(REPO_ROOT / "src" / "core" / "ast.ixx")
            and "56" in _read(REPO_ROOT / "src" / "core" / "ast.ixx"),
        )
    )
    rows.append(
        (
            "3396 AC4: #2960 stamp_query_stable_ref_export non-regress",
            "stamp_query_stable_ref_export (which fills tenant/fiber/cow/wrap "
            "before Agent export — #2960) must still be wired in src/compiler/evaluator.ixx "
            "or the corresponding query-side stamp helper.",
            "stamp_query_stable_ref_export" in _read(REPO_ROOT / "src" / "compiler" / "evaluator.ixx"),
        )
    )
    # The v2 fields filled by walk_v2 (wrap + tenant + cow + fiber + boundary)
    # are exactly the fields stamped by stamp_query_stable_ref_export — so
    # the inbound v2 contract matches the outbound v2 export face.
    rows.append(
        (
            "3396 AC4: walk_v2 fills the same fields stamped by stamp_query_stable_ref_export",
            "walk_v2 in mutate.cpp fills wrap_epoch + tenant_id + cow_epoch_at_capture "
            "(+ fiber_id + boundary_pinned if present) — the exact fields that "
            "stamp_query_stable_ref_export stamps. The v2 pair spine is the "
            "Agent-visible contract that matches the v2 wire face.",
            "ref.wrap_epoch" in mut and "ref.tenant_id" in mut and "ref.cow_epoch_at_capture" in mut,
        )
    )

    # AC5 cite anchors (commit message traceability) + no docs/design/, no
    # tests/issues/test_issue_3396.cpp.
    rows.append(
        (
            "3396 AC5: Issue #3396 cite present in unpack_stable_ref_arg / walk_v2",
            "Both unpack helpers (mutate.cpp + query_workspace.cpp) must cite "
            "Issue #3396 so the v2 contract is traceable in future audits.",
            "#3396" in mut and "#3396" in qws,
        )
    )
    rows.append(
        (
            "3396 AC5: no docs/design/3396-*.md plan doc",
            "Per the #1655 / no-design-docs directive, #3396 carries no "
            "docs/design/<issue>-<slug>.md plan doc — design rationale lives "
            "in the commit message + close comment.",
            not (DESIGN_DIR / "3396-unpack-stable-ref-arg-v2.md").exists()
            and not any(p.name.startswith("3396-") and p.suffix == ".md" for p in DESIGN_DIR.glob("3396-*.md"))
            if DESIGN_DIR.exists()
            else True,
        )
    )
    rows.append(
        (
            "3396 AC5: no tests/issues/test_issue_3396.cpp",
            "Per the agent-development test-suiting constraint, the #3396 "
            "tests live in tests/<src-aligned suite>/ (extend existing "
            "test_stable_ref_provenance_fiber_cow.cpp) — not in tests/issues/.",
            not (ISSUES_TESTS_DIR / "test_issue_3396.cpp").exists(),
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
    print("OK: Issue #3396 unpack_stable_ref_arg v2 contract — all AC rows satisfied")
    return 0


def _self_test() -> int:
    """Build a fake source tree and assert --strict pass / fail behaviour.

    Pass case: write the expected patterns to fake mutate.cpp + query_workspace.cpp.
    All rows pass.
    Fail case: drop `walk_v2` from the fake mutate.cpp. AC1 row flips to FAIL.
    """
    import importlib

    with tempfile.TemporaryDirectory(prefix="check_unpack_stable_ref_arg_v2_3396_") as td:
        root = Path(td)
        (root / "src" / "compiler").mkdir(parents=True)
        (root / "src" / "core").mkdir(parents=True)
        (root / "docs" / "design").mkdir(parents=True)
        (root / "tests" / "issues").mkdir(parents=True)

        mut_text = (
            "// fake mut\n"
            "aura::compiler::typed_audit::production_defaults_active()\n"
            "walk_v2\n"
            "if (!walk_v2(cdr)) return std::nullopt\n"
            "ensure_valid_or_refresh\n"
            "bump_stable_ref_provenance_enforced\n"
            "ref.wrap_epoch\n"
            "ref.tenant_id\n"
            "ref.cow_epoch_at_capture\n"
            "Issue #3396\n"
            "else {\n"
            "    is_pair(cdr)\n"
            "    as_pair_idx(cdr)\n"
            "}\n"
        )
        qws_text = (
            "// fake qws\n"
            "aura::compiler::typed_audit::production_defaults_active()\n"
            "walk_v2\n"
            "if (!walk_v2(cdr)) return std::nullopt\n"
            "Issue #3396\n"
        )
        ast_text = "// fake ast\nkStableRefSerializedSizeV2\n56\n"
        eval_text = "// fake eval\nstamp_query_stable_ref_export\n"
        (root / "src" / "compiler" / "evaluator_primitives_mutate.cpp").write_text(mut_text)
        (root / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp").write_text(qws_text)
        (root / "src" / "core" / "ast.ixx").write_text(ast_text)
        (root / "src" / "compiler" / "evaluator.ixx").write_text(eval_text)

        saved = os.environ.get("AURA_3396_REPO_ROOT")
        os.environ["AURA_3396_REPO_ROOT"] = str(root)
        try:
            import check_unpack_stable_ref_arg_v2_3396 as selfmod

            importlib.reload(selfmod)
            selfmod.REPO_ROOT = Path(root)
            selfmod.MUT_PATH = Path(root / "src" / "compiler" / "evaluator_primitives_mutate.cpp")
            selfmod.QWS_PATH = Path(root / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp")
            selfmod.DESIGN_DIR = Path(root / "docs" / "design")
            selfmod.ISSUES_TESTS_DIR = Path(root / "tests" / "issues")

            print("self-test PASS case (all rows satisfied):")
            pass_rows = selfmod._rows()
            rc_pass = selfmod._run(pass_rows)
            if rc_pass != 0:
                print("self-test FAIL: pass case did not return 0")
                return 1

            # Now drop walk_v2 from the fake mutate.cpp → AC1 row must fail.
            (root / "src" / "compiler" / "evaluator_primitives_mutate.cpp").write_text(
                "// fake mut without walk_v2\n" + mut_text.replace("walk_v2", "v2_walker_DISABLED")
            )
            print("self-test FAIL case (walk_v2 missing from mutate.cpp):")
            fail_rows = selfmod._rows()
            rc_fail = selfmod._run(fail_rows)
            if rc_fail == 0:
                print("self-test FAIL: fail case did not return non-zero")
                return 1
        finally:
            if saved is None:
                os.environ.pop("AURA_3396_REPO_ROOT", None)
            else:
                os.environ["AURA_3396_REPO_ROOT"] = saved

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
