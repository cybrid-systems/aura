#!/usr/bin/env python3
"""Issue #4088: bare NodeId still returned by several query:* on the
production face; later reads take the occupied slot's value (P0).

Hole: end_query_epoch_maybe_result (query_workspace) auto-upgrades
find / pattern / filter / by-marker / children / parent to schema-2 under
production, but five list exits bypass it and hand Agent memory a bare
int-linked-list of NodeIds — occupancy, not identity. After slot reuse the
Agent reads the NEW occupant: query:defines / query:calls / query:node-type
/ query:defines-by-marker / query:calls-by-marker (query_workspace), plus
query:reflect-node-members' body-node / init-node members (make_int(child))
and query:dirty-subtree's bare-int root walked lock-free over the borrowed
FlatAST::children span.

Fix shape (no new query API, no old query key change):
  - the five list exits finish via end_query_epoch_maybe_result
    (Production auto-upgrades to the schema-2 stamped hash whose matches
    carry reserved == kQueryResultMatchSchema2Prod; Soft keeps the bare
    list — the helper handles both faces);
  - reflect-node-members body/init members export the same stable-ref
    spine as query:as-stable-ref (Soft v1 (id . gen); production v2
    spine, pack_v2 field order), no new primitive;
  - query:dirty-subtree re-registers from the workspace registration
    (Primitives::add overrides by name; register_workspace_query_
    primitives runs after register_query_primitives): production resolves
    the root through resolve_query_node_arg (bare int → stale-ref, the
    #3395 gate), takes the workspace shared lock, and walks
    children_columnar; Soft keeps the historical lock-free bare-int body.
  - query:ref-counts is deliberately NOT in scope (the issue's fix list
    does not name it).

Contract (one row per AC):
  AC1  the five list exits finish via end_query_epoch_maybe_result with
       as_query_result=false and cite #4088; the bare defines finish is
       gone
  AC2  the Soft contract is intact: the helper's production auto-upgrade
       SSOT and the bare-list early return are retained (Soft keeps bare
       lists, zero-cost)
  AC3  reflect-node-members exports body/init through the as-stable-ref
       spine (pack_member_ref + budget gate + production v2 gate; no
       make_int member export)
  AC4  dirty-subtree production face resolves the root (#3395 stale-ref
       gate), takes workspace shared_lock, walks children_columnar; Soft
       keeps the historical walk; the override order precondition holds
  AC5  build.py wires check_query_bare_nodeid_4088 + root allowlist; the
       runtime ACs live in test_query_result_full_provenance.cpp; no
       tests/**/test_issue_4088.cpp; no docs/design/4088-*

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

    def window(src: str, begin: str, end: str, label: str, width: int = 4200) -> str:
        b = src.find(begin)
        if b == -1:
            fails.append(f"{label}: anchor {begin!r} missing")
            return ""
        e = src.find(end, b + len(begin))
        return src[b : (e if e != -1 else b + width)]

    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    qreg = _read("src/compiler/evaluator_primitives_registry.cpp")
    qold = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_query_result_full_provenance.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: the five list exits finish via end_query_epoch_maybe_result ─
    exits = [
        ('add("query:calls"', 'add("query:defines"', "query:calls"),
        ('add("query:defines"', '["query:parent"]', "query:defines"),
        ('add("query:node-type"', 'add("query:node-marker"', "query:node-type"),
        (
            'add("query:defines-by-marker"',
            'add("query:calls-by-marker"',
            "query:defines-by-marker",
        ),
        (
            'add("query:calls-by-marker"',
            '["query:by-marker"]',
            "query:calls-by-marker",
        ),
    ]
    for begin, end, name in exits:
        body = window(qws, begin, end, f"AC1 {name} window")
        must(
            "end_query_epoch_maybe_result(qe, &flat, result, /*as_query_result=*/false)",
            f"AC1 {name} schema-2 finish",
            body,
        )
        must("Issue #4088", f"AC1 {name} cites the issue", body)
        absent("return result;\n    });", f"AC1 {name} bare exit gone", body)
    defines_body = window(qws, 'add("query:defines"', '["query:parent"]', "AC1 defines")
    absent("return end_query_epoch(qe, &flat, result);", "AC1 defines bare end_query_epoch finish gone", defines_body)

    # ── AC2: the Soft contract is intact (auto-upgrade SSOT + early ret) ─
    must("as_query_result = true; // auto-upgrade", "AC2 auto-upgrade SSOT", qws)
    upgrade = qws.find("as_query_result = true; // auto-upgrade")
    soft_early = qws.find("return finished;", upgrade) if upgrade != -1 else -1
    if upgrade == -1 or soft_early == -1 or soft_early - upgrade > 400:
        fails.append("AC2: Soft bare-list early return must follow the auto-upgrade")

    # ── AC3: reflect-node-members exports the as-stable-ref spine ────────
    reflect = window(qws, "query:reflect-node-members", "query:ref-counts", "AC3 reflect window")
    must("Issue #4088", "AC3 reflect cites the issue", reflect)
    must("pack_member_ref", "AC3 reflect spine packer", reflect)
    must("allow_query_stable_ref_export(child)", "AC3 export budget gate", reflect)
    must("production_defaults_active()", "AC3 production v2 gate", reflect)
    must("query:as-stable-ref", "AC3 as-stable-ref parity cite", reflect)
    absent('append_field("body-node", make_int', "AC3 bare body-node gone", reflect)
    absent('append_field("init-node", make_int', "AC3 bare init-node gone", reflect)

    # ── AC4: dirty-subtree production face + override precondition ───────
    dirty = window(qws, 'add("query:dirty-subtree"', '"query:defines-by-marker"', "AC4 dirty window")
    must("Issue #4088", "AC4 dirty cites the issue", dirty)
    must('resolve_query_node_arg(a, "query:dirty-subtree"', "AC4 root resolve (#3395)", dirty)
    must("rlock(ws.workspace_mtx)", "AC4 workspace shared lock", dirty)
    must("children_columnar", "AC4 columnar children walk", dirty)
    must("production_defaults_active()", "AC4 production gate", dirty)
    must("ws_flat->children(cur)", "AC4 Soft keeps the historical walk", dirty)
    must("Evaluator::get_query_evaluator()", "AC4 Soft keeps the historical lookup", dirty)
    # The override precondition: the bare registration must still exist in
    # evaluator_primitives_query.cpp and the workspace registration must
    # run after register_query_primitives.
    must('add("query:dirty-subtree"', "AC4 overridden bare registration retained", qold)
    bare_reg = qreg.find("register_query_primitives(")
    ws_reg = qreg.find("register_workspace_query_primitives(")
    if bare_reg == -1 or ws_reg == -1 or bare_reg > ws_reg:
        fails.append(
            "AC4: registry must call register_query_primitives before "
            "register_workspace_query_primitives (Primitives::add override order)"
        )

    # ── AC5: wiring + test extension + no invented files ─────────────────
    must("check_query_bare_nodeid_4088", "AC5 build.py wires the linter", build)
    must("check_query_bare_nodeid_4088.py", "AC5 root allowlist carries the linter", allow)
    must("test_ac4088_1_prod_list_exits_schema2", "AC5 runtime AC1 present", test)
    must("test_ac4088_2_prod_dirty_subtree_stale_ref", "AC5 runtime AC2 present", test)
    must("test_ac4088_3_soft_list_exits_bare", "AC5 runtime AC3 present", test)
    must("test_ac4088_4_soft_dirty_subtree_int_ok", "AC5 runtime AC4 present", test)
    must("test_ac4088_5_soft_reflect_member_pair", "AC5 runtime AC5 present", test)
    must("test_ac4088_6_source_cite", "AC5 source-cite AC present", test)
    for fn in (
        "test_ac4088_1_prod_list_exits_schema2",
        "test_ac4088_2_prod_dirty_subtree_stale_ref",
        "test_ac4088_3_soft_list_exits_bare",
        "test_ac4088_4_soft_dirty_subtree_int_ok",
        "test_ac4088_5_soft_reflect_member_pair",
        "test_ac4088_6_source_cite",
    ):
        main_tail = test[test.find("int main() {") :] if "int main() {" in test else ""
        must(f"{fn}();", f"AC5 {fn} dispatched from main", main_tail)
    if _read("tests/compiler/test_issue_4088.cpp"):
        fails.append("AC5: tests/compiler/test_issue_4088.cpp must not exist (#81934)")
    if _read("tests/core/test_issue_4088.cpp"):
        fails.append("AC5: tests/core/test_issue_4088.cpp must not exist (#81934)")
    if (ROOT / "docs" / "design").is_dir() and list((ROOT / "docs" / "design").glob("4088-*")):
        fails.append("AC5: docs/design/4088-* must not exist (#1655)")

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("check_query_bare_nodeid_4088: OK (5 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
