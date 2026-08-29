#!/usr/bin/env python3
"""Issue #3131: P0 residual — scalar/metadata mutate prims (replace-value /
replace-type / record-patch) bypass MacroIntroduced default-deny.

Structural mutate prims already route through
reject_structural_macro_hygiene / hygiene_protected_error and respect
:allow-macro? / get_allow_macro_mutate(). Scalar/metadata paths do not.

Contract:
  AC1 mutate:record-patch body calls reject_structural_macro_hygiene on
     the target node BEFORE flat.add_mutation, captures was_macro, and
     on allowed path calls propagate_macro_introduced_marker.
  AC2 mutate:replace-type / replace-value body still call
     reject_structural_macro_hygiene (regression — #3115 fix preserved).
  AC3 mutate:tweak-literal body still calls hygiene_protected_error
     (regression — pre-existing fix preserved).
  AC4 Stable Agent-visible reason remains "hygiene" /
     "hygiene-protected"; reject_structural_macro_hygiene mev("hygiene", …)
     and hygiene_protected_error mev("hygiene-protected", …) unchanged.
  AC5 No new metric keys — reject_structural_macro_hygiene bumps the
     existing naked_macro_mutate_attempt + macro_hygiene_provenance_hits_total
     and sets last_hygiene_blame_node (no new atomics).
  AC6 Regression test in tests/compiler/ (src/-aligned, no
     tests/issues/test_issue_3131.cpp); no docs/design/3131-* plan doc
     (#1655 — agent repo philosophy).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _window(src: str, start_token: str, end_token: str) -> str:
    """Return substring of src between start_token and end_token (exclusive)."""
    a = src.find(start_token)
    if a == -1:
        return ""
    b = src.find(end_token, a + len(start_token))
    if b == -1:
        # Fallback: take a generous tail so partial-window lints still fire.
        return src[a : a + 8000]
    return src[a:b]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_all(tokens: list[str], label_prefix: str, hay: str) -> None:
        for t in tokens:
            must(t, f"{label_prefix}", hay)

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    q = read_query_prims()

    # ── AC1: record-patch body — guard + restamp + opt-out ──
    # Anchor on the unique 'Issue #3131' stamp instead of the
    # `add_mutate("mutate:record-patch"` prefix — clang-format may split
    # the call across lines (e.g. `add_mutate(\n    "mutate:record-patch",`)
    # which would break literal substring window extraction. Variable
    # names (allow_macro_rp, was_macro_rp) are unique to this block.
    rp_anchor = mut.find("Issue #3131")
    if rp_anchor == -1:
        fails.append("AC1: 'Issue #3131' stamp missing in evaluator_primitives_mutate.cpp")
        rp = ""
    else:
        rp_end = mut.find("mutate:query-and-replace", rp_anchor)
        if rp_end == -1:
            rp_end = rp_anchor + 6000
        rp = mut[rp_anchor:rp_end]
    must_all(
        [
            "Issue #3131",
            "reject_structural_macro_hygiene(ev, flat, node, allow_macro_rp",
            "was_macro_rp",
            "parse_allow_macro_opt_out(ev, a)",
            "propagate_macro_introduced_marker(ev, flat, node,",
            "parse_no_auto_restamp_opt_out(ev, a)",
            '"record-patch"',  # prim name passed to reject_structural_macro_hygiene
        ],
        "AC1 record-patch body",
        rp,
    )
    # Guard must be the FIRST thing after node resolve + out-of-range check.
    # #3399 routes the occupancy index through resolve_mutate_node_arg
    # (packed v2 ref / QueryResult) instead of a bare static_cast.
    pos_resolve = rp.find("auto node = static_cast<aura::ast::NodeId>")
    if pos_resolve < 0:
        pos_resolve = rp.find("resolve_mutate_node_arg")
    pos_guard = rp.find("reject_structural_macro_hygiene(ev, flat, node, allow_macro_rp")
    pos_add = rp.find("flat.add_mutation(node,")
    if pos_resolve == -1 or pos_guard == -1 or pos_add == -1:
        fails.append("AC1: missing node-resolve / guard / add_mutation markers")
    elif not (pos_resolve < pos_guard < pos_add):
        fails.append(
            "AC1: guard must sit between node resolve and flat.add_mutation "
            f"(pos_resolve={pos_resolve}, pos_guard={pos_guard}, pos_add={pos_add})"
        )

    # ── AC2: replace-type / replace-value still call guard (#3115 preserved) ──
    # Variable names (allow_macro_rt, allow_macro_rv) are unique per-block —
    # substring match on the full file is sufficient.
    must("reject_structural_macro_hygiene(ev, flat, node, allow_macro_rt", "AC2 replace-type body", mut)
    must("reject_structural_macro_hygiene(ev, flat, node, allow_macro_rv", "AC2 replace-value body", mut)
    must("Issue #3115", "AC2 #3115 cite", mut)

    # ── AC3: tweak-literal still calls hygiene_protected_error ──
    must("hygiene_protected_error", "AC3 tweak-literal guard", mut)
    must("Issue #373", "AC3 tweak-literal cite", mut)

    # ── AC4: stable reasons — no new strings introduced ──
    must('mev("hygiene",', "AC4 hygiene reason", mut)
    must('mev("hygiene-protected",', "AC4 hygiene-protected reason", mut)
    # Specifically: reject_structural_macro_hygiene returns "hygiene".
    helper = _window(
        mut,
        "static std::optional<EvalValue> reject_structural_macro_hygiene(",
        "static aura::ast::NodeId first_macro_introduced_in_subtree(",
    )
    must('mev("hygiene",', "AC4 reject_structural_macro_hygiene returns hygiene", helper)
    must("cannot ", "AC4 reject_structural_macro_hygiene reason phrase", helper)
    if "Issue #3131" in mut and 'mev("hygiene-3131"' in mut:
        fails.append("AC4: new reason string 'hygiene-3131' introduced (forbidden)")

    # ── AC5: counter surface unchanged — existing keys still bumped ──
    must_all(
        [
            "naked_macro_mutate_attempt",
            "macro_hygiene_provenance_hits_total",
            "last_hygiene_blame_node",
        ],
        "AC5 reject counters",
        mut,
    )
    must_all(
        [
            "naked_macro_mutate_attempt",
            "macro_hygiene_provenance_hits_total",
            "last_hygiene_blame_node",
        ],
        "AC5 metrics struct",
        obs,
    )
    if "schema-3131" in q:
        fails.append("AC5: new query key schema-3131 (forbidden)")

    # ── AC6: src/-aligned test, no tests/issues/test_issue_3131, no plan doc ──
    must(
        "Issue #3131", "AC6 regression test cites", _read("tests/compiler/test_scalar_mutate_record_patch_hygiene.cpp")
    )
    must(
        "reject_structural_macro_hygiene",
        "AC6 regression test asserts guard",
        _read("tests/compiler/test_scalar_mutate_record_patch_hygiene.cpp"),
    )
    if (ROOT / "tests" / "issues" / "test_issue_3131.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3131.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3131.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3131.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3131-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    # Coverage wiring — linter must be reachable from build.py / pyproject /
    # coverage scanner (mirror #3130 / #3118). If we ever migrate coverage
    # wiring, this check ensures the linter doesn't get orphaned.
    must(
        "check_scalar_mutate_hygiene_3131",
        "AC6 build.py",
        _read("build.py") + _read("pyproject.toml") + _read(".pre-commit-config.yaml"),
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3131 scalar/metadata mutate MacroIntroduced hygiene — all 6 AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
