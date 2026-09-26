#!/usr/bin/env python3
"""Issue #4106: production query prims still treat a bare NodeId as the
current occupant (P1).

Hole: under production defaults the main query loop resolves operands
through resolve_query_node_arg (bare int → stale-ref, #3395) and finishes
match lists via end_query_epoch_maybe_result (schema-2 auto-upgrade,
#3286/#3449) — but a set of public query primitives bypass both gates:

  - query:stable-ref mints a schema-2 hash of whoever occupies a bare int
    (a remembered int becomes fresh memory);
  - query:ensure-ref stamps the current slot for a bare int and refreshes a
    stale packed gen across wrap_epoch=0 + export_held_ref auto-refresh;
  - query:stable-ref-provenance stamps the current slot for a bare int
    (schema-620, is-live) with no workspace_mtx shared lock;
  - query:reaches walks reaches_for_node of the bare int slot;
  - query:ref-counts counts parents of the bare int slot;
  - query:macro-introduced hands Agent memory a bare NodeId list with no
    epoch bracket (sunk body, #3175 — hardening for future surfacing);
  - query:macro-provenance-chain walks provenance of the bare int slot with
    no shared lock (sunk body, #3175 — hardening for future surfacing).

Fix shape (reuse the existing gates; no new primitive, no new query key):
  - stable-ref / ensure-ref / ref-counts (workspace TU): production resolves
    the operand through resolve_query_node_arg / refuse_valid0 (the #3395
    face in each primitive's own shape); packed v2 / schema-2 operands
    resolve + validate with the non-refresh rule (#3661). Soft keeps the
    historical bare-int bodies.
  - stable-ref-provenance (query.cpp), reaches (defuse TU),
    macro-provenance-chain (lifecycle TU): same contract mirrored through
    the shared unpack_query_stable_ref_v2 + resolve_query_result_match
    helpers; the production read takes the workspace shared lock
    (Evaluator::WorkspaceSharedLock).
  - macro-introduced: production brackets the match list with
    begin_query_epoch + end_query_epoch_maybe_result (schema-2 or structured
    overflow / restamp-lag). Soft keeps the bare list.
  - ensure-ref packed gen mismatch: never zeroes wrap_epoch nor auto-
    refreshes under production — the diagnostic valid=0 face (refreshed=0).

Contract (one row per AC):
  AC1  stable-ref production resolves the operand (#3395 stale-ref face;
       packed v2 / schema-2 accepted); soft keeps the bare-int mint with
       export_ref_safe + the #3862 schema-2 bracket
  AC2  ensure-ref: bare int → valid=0 diagnostic refuse (no stamp); packed
       gen mismatch keeps wrap_epoch and refuses valid=0 refreshed=0 before
       export_held_ref; schema-2 hash resolves; soft refresh retained
  AC3  stable-ref-provenance: bare int → #f; packed v2 / schema-2 resolve
       through the shared helpers; production takes WorkspaceSharedLock;
       the #3287 torn gate is retained
  AC4  reaches: production resolves before the walk (stale-ref face, shared
       helpers, current Evaluator); soft keeps cb.reaches_for_node
  AC5  ref-counts: production resolves before the parent walk; soft keeps
       the bare-int walk (out-of-range → 0)
  AC6  macro-introduced production epoch bracket + macro-provenance-chain
       resolve/lock hardening in the sunk bodies (#3175 compose)
  AC7  shared unpack_query_stable_ref_v2 in query_result_decode.hh; build.py
       + root allowlist wiring; runtime ACs dispatched in the three suites;
       no tests/**/test_issue_4106.cpp; no docs/design/4106-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    def window(src: str, begin: str, end: str, label: str, width: int = 5200) -> str:
        b = src.find(begin)
        if b == -1:
            fails.append(f"{label}: anchor {begin!r} missing")
            return ""
        e = src.find(end, b + len(begin))
        return src[b : (e if e != -1 else b + width)]

    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    qprov = _read("src/compiler/evaluator_primitives_query.cpp")
    qdef = _read("src/compiler/evaluator_primitives_query_defuse.cpp")
    qlife = _read("src/compiler/evaluator_primitives_query_lifecycle.cpp")
    dec = _read("src/compiler/query_result_decode.hh")
    epoch_test = _read("tests/compiler/test_query_epoch_contract.cpp")
    cow_test = _read("tests/serve/test_stable_ref_provenance_fiber_cow.cpp")
    hyg_test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: stable-ref production resolve + soft mint retained ──────────
    sr = window(qws, 'add("query:stable-ref"', 'add("query:ensure-ref"', "AC1 stable-ref")
    must("Issue #4106", "AC1 cites the issue", sr)
    must("production_defaults_active()", "AC1 production gate", sr)
    must('resolve_query_node_arg(a, "query:stable-ref"', "AC1 operand resolve (#3395)", sr)
    must("export_ref_safe(node", "AC1 soft keeps export_ref_safe mint", sr)
    must(
        "end_query_epoch_maybe_result(qe, &flat, packed, /*as_query_result=*/false)",
        "AC1 #3862 schema-2 bracket retained",
        sr,
    )

    # ── AC2: ensure-ref refusal faces + non-refresh rule ─────────────────
    ens = window(qws, 'add("query:ensure-ref"', 'add("query:ref-valid?"', "AC2 ensure-ref")
    must("Issue #4106", "AC2 cites the issue", ens)
    must("refuse_valid0", "AC2 shared valid=0 refusal builder", ens)
    must(
        "return refuse_valid0(node, /*gen=*/0, /*from_packed=*/false);",
        "AC2 bare-int production refuse (no stamp)",
        ens,
    )
    must(
        "if (!aura::compiler::typed_audit::production_defaults_active())\n                    stamped.wrap_epoch = 0;",
        "AC2 wrap_epoch=0 gated to Soft legacy pack",
        ens,
    )
    must(
        "if (aura::compiler::typed_audit::production_defaults_active() && !was_valid)\n"
        "            return refuse_valid0(held.id, held.gen, from_packed);",
        "AC2 packed stale non-refresh before export_held_ref",
        ens,
    )
    must('resolve_query_node_arg(a, "query:ensure-ref"', "AC2 schema-2 hash resolve", ens)
    must("auto exported = ev.export_held_ref(held);", "AC2 export_held_ref retained", ens)

    # ── AC3: stable-ref-provenance production face + shared lock ─────────
    prov = window(
        qprov,
        '"query:stable-ref-provenance"',
        '"query:stable-ref-lifecycle-stats"',
        "AC3 provenance",
    )
    must("Issue #4106", "AC3 cites the issue", prov)
    must("Evaluator::WorkspaceSharedLock", "AC3 workspace shared lock", prov)
    must("unpack_query_stable_ref_v2", "AC3 shared v2 unpack", prov)
    must("resolve_query_result_match", "AC3 schema-2 hash resolve", prov)
    must(
        "ev.ensure_valid_or_refresh(ref, /*auto_refresh=*/false)",
        "AC3 non-refresh rule (#3661)",
        prov,
    )
    must("prod_4106 && !ev.allow_query_stable_ref_export(nid)", "AC3 #3287 torn gate", prov)

    # ── AC4: reaches production resolve before the walk ──────────────────
    rea = window(qdef, 'add("query:reaches"', 'add("query:effects"', "AC4 reaches")
    must("Issue #4106", "AC4 cites the issue", rea)
    must("raw node-id rejected under production", "AC4 #3395 stale-ref face", rea)
    must("Evaluator::get_query_evaluator()", "AC4 current-Evaluator lookup", rea)
    must("unpack_query_stable_ref_v2", "AC4 shared v2 unpack", rea)
    must("resolve_query_result_match", "AC4 schema-2 hash resolve", rea)
    must("cb.reaches_for_node(idx, target)", "AC4 soft walk retained", rea)

    # ── AC5: ref-counts production resolve before the parent walk ────────
    rc = window(qws, '"query:ref-counts"', 'add("query:dirty-subtree"', "AC5 ref-counts")
    must("Issue #4106", "AC5 cites the issue", rc)
    must('resolve_query_node_arg(a, "query:ref-counts"', "AC5 operand resolve", rc)
    must("production_defaults_active()", "AC5 production gate", rc)
    must("return make_int(0);", "AC5 soft out-of-range → 0 retained", rc)

    # ── AC6: sunk-body hardening (#3175 compose) ─────────────────────────
    mi = window(
        qws,
        # Issue #4106: clang-format re-wrapped the registration to a multi-line
        # form; anchor on the quoted name (first quoted occurrence).
        '"query:macro-introduced"',
        '"query:marker-stats"',
        "AC6 macro-introduced",
    )
    must("Issue #4106", "AC6 macro-introduced cites the issue", mi)
    must("production_defaults_active()", "AC6 macro-introduced production gate", mi)
    must("begin_query_epoch(&flat)", "AC6 macro-introduced epoch begin", mi)
    must(
        "end_query_epoch_maybe_result(qe, &flat, result,",
        "AC6 macro-introduced schema-2 finish (call)",
        mi,
    )
    must("/*as_query_result=*/false", "AC6 macro-introduced bare-list exit gone", mi)
    mpc = window(
        qlife,
        '"query:macro-provenance-chain"',
        "FlatHashTable::create(48)",
        "AC6 macro-provenance-chain",
    )
    must("Issue #4106", "AC6 chain cites the issue", mpc)
    must("unpack_query_stable_ref_v2", "AC6 chain v2 unpack", mpc)
    must("Evaluator::WorkspaceSharedLock", "AC6 chain shared lock", mpc)
    must("resolve_query_result_match", "AC6 chain schema-2 hash resolve", mpc)

    # ── AC7: shared helper + wiring + runtime ACs + no invented files ────
    must("unpack_query_stable_ref_v2", "AC7 shared helper present", dec)
    must("Issue #4106", "AC7 helper cites the issue", dec)
    must("check_query_bare_nodeid_4106", "AC7 build.py wires the linter", build)
    must("check_query_bare_nodeid_4106.py", "AC7 root allowlist carries the linter", allow)
    must("#4106: query:macro-introduced never a bare int list", "AC7 epoch-suite AC present", epoch_test)
    for fn in (
        "ac4106_1_prod_mint_bare_int_refuses",
        "ac4106_2_prod_packed_and_hash_still_resolve",
        "ac4106_3_recycled_slot_never_rebound",
        "ac4106_4_soft_stable_ref_still_mints",
        "ac4106_5_no_docs_linter_wired",
    ):
        must(f"{fn}();", f"AC7 {fn} dispatched", cow_test)
    must("ac4106_read_trio();", "AC7 hygiene AC dispatched", hyg_test)
    must("ac4106_read_trio", "AC7 hygiene AC defined", hyg_test)
    if _read("tests/compiler/test_issue_4106.cpp"):
        fails.append("AC7: tests/compiler/test_issue_4106.cpp must not exist (#81934)")
    if _read("tests/serve/test_issue_4106.cpp"):
        fails.append("AC7: tests/serve/test_issue_4106.cpp must not exist (#81934)")
    if (ROOT / "docs" / "design").is_dir() and list((ROOT / "docs" / "design").glob("4106-*")):
        fails.append("AC7: docs/design/4106-* must not exist (#1655)")

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("check_query_bare_nodeid_4106: OK (7 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
