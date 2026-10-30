#!/usr/bin/env python3
"""Issue #4598 (phase 3): persistent snapshot — O(1) share + O(changed) stamp restore.

Follow-up to the #4598 phase-2 commit (boundary-exit counters + macro gate).
The pinned residual on the hot-swap path that this slice removes:

  - ast:snapshot deep-copied every FlatAST column (O(workspace)) on every
    call. Snapshots are now shared, frozen copies: a snapshot whose
    generation, size and pool mint watermark match a prior capture shares
    that copy (the rebind → restore loop lands exactly there). The live
    object is never shared — it keeps mutating in place.
  - ast:restore unparsed every define twice (collect_reachable_define_sources
    live + snapshot) to build the diff. The diff is now per-define
    subtree_gen stamps: mark_dirty_upward attributes every guard-window
    content change to the enclosing top-level Define's subtree_gen_ cell
    (light note_subtree_content_write bump), so an unchanged define costs
    one integer compare and only the dirty cone is unparsed — lazily, once,
    memoized on the shared stamps.
  - The #4360 / #4349 IR-closure-owner discard and the production mark-all
    fallback are preserved: the stamp diff feeds the same changed/added/
    removed machinery, and a pre-stamp entry falls back to the full unparse
    diff.

Contract:
  AC1 FlatAST light per-define change attribution:
      note_subtree_content_write bumps the owning top-level Define's
      subtree_gen_ cell (no global generation_ bump, no restamp) and
      mark_dirty_upward calls it — the soundness anchor.
  AC2 snapshot share: FlatSnapshot holds shared_ptr copies; the probe
      requires generation + size + pool watermark match, the same workspace
      source text, and a FROZEN copy (never the live object); hit/miss
      counters recorded. (The source-text match is the content identity: a
      set-code allocates a fresh FlatAST whose generation restarts at 1.)
  AC3 stamp capture at snapshot: collect_define_stamps walks the LIVE flat
      (name → root, gen16) on the deep path only; shared entries reuse the
      prior stamps.
  AC4 restore stamp diff: live-side collect_define_change_stamps with no
      unparse; gen16 equality skips the define; sources lazily unparsed
      once from the frozen capture and memoized (stamp counters exposed).
  AC5 #4360/#4349 semantics preserved: drop_ir_define_binding_fn_ cascade,
      mark_define_dirty_fn_, kRestoreDiffCap and the production mark-all
      branch all still driven by the diff.
  AC6 query:workspace-snapshot-stats exposes the four new counters and the
      table capacity grew for them.
  AC7 runtime ACs live in the existing hosts (test_summary_recompute_sym.cpp
      #4598 AC11; test_current_source_roundtrip.cpp phase-3 function) — no
      tests/core/test_issue_4598.cpp.
  AC8 build.py wiring + root_check_allowlist entry; no docs/design/4598-*.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    ast = _read("src/core/ast.ixx")
    ast_impl = _read("src/core/ast_impl.cpp")
    evixx = _read("src/compiler/evaluator.ixx")
    prim = _read("src/compiler/evaluator_primitives_ast.cpp")
    prim_eval = _read("src/compiler/evaluator_primitives_eval.cpp")
    obs = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    sym_test = _read("tests/core/test_summary_recompute_sym.cpp")
    round_test = _read("tests/compiler/test_current_source_roundtrip.cpp")
    build_py = _read("build.py")
    allowlist = _read("scripts/coverage/root_check_allowlist.txt")

    # AC1 light per-define change attribution
    must("void note_subtree_content_write(NodeId node) noexcept;", "AC1", ast)
    must(
        "Issue #4598 phase 3: light per-define content-write note.",
        "AC1",
        ast,
    )
    must("void FlatAST::note_subtree_content_write(NodeId node) noexcept {", "AC1", ast_impl)
    must("const auto top = top_define_of(node);", "AC1", ast_impl)
    must("note_subtree_content_write(id);", "AC1", ast_impl)
    must("Issue #4598 phase 3: attribute the write to its enclosing top-level", "AC1", ast_impl)
    # light = no global generation bump / no restamp inside the note path
    must_not("restamp_subtree_generation(subtree_root);\n    // Issue #4598 phase 3", "AC1", ast_impl)

    # AC2 snapshot share — frozen copies only
    must("std::shared_ptr<const aura::ast::FlatAST::Image> flat;", "AC2", evixx)  # #4772 COW image
    must("std::shared_ptr<const aura::ast::StringPool::Image> pool;", "AC2", evixx)
    must("std::size_t pool_size = 0;", "AC2", evixx)
    must("fs.flat = ev.workspace_flat_->capture_image();", "AC2", prim)
    must("fs.pool = ev.workspace_pool_->capture_image();", "AC2", prim)
    # #4772: share hits are exact by construction (no copied content chunk)
    must("if (fs.flat->content_fresh_chunks == 0)", "AC2", prim)
    # Issue #4613 residual: set-code allocates a FRESH FlatAST whose
    # generation restarts at 1 and whose node count can be unchanged when
    # only a literal changes — the triple above is not a content identity
    # across set-code. A share needs the same workspace source text too,
    # or an older snapshot's frozen copy is served for new source.
    # #4613 set-code case: a fresh FlatAST has no base image, so it always copies
    must("the #4613 set-code case is a fresh FlatAST with no base", "AC2", prim)
    must("++ev.ast_snapshot_share_hits_total_;", "AC2", prim)
    must("++ev.ast_snapshot_share_misses_total_;", "AC2", prim)
    must(
        "std::uint64_t ast_snapshot_share_hits_total_{0};",
        "AC2",
        evixx,
    )
    must("[[nodiscard]] std::uint64_t ast_snapshot_share_hits_total() const noexcept", "AC2", evixx)

    # AC3 stamp capture at snapshot
    # #4772: stamps resolve lazily at first restore from the image cells
    must("void resolve_lazy_define_stamps(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,", "AC3", prim)
    must("st.gen16 = img.subtree_gen_at(cur);", "AC3", prim)
    must("st.ver = img.define_ver_at(cur); // Issue #4792", "AC3", prim)
    must("fs.stamps = std::make_shared<FlatSnapshotDefineStamps>();", "AC3", prim)
    must("fs.stamps->lazy = true;", "AC3", prim)
    must("std::unordered_map<std::string, DefineStamp> by_name;", "AC3", evixx)

    # AC4 restore stamp diff — no full unparse on the hot path
    must(
        "collect_define_change_stamps(aura::ast::FlatAST& flat, aura::ast::StringPool& pool)",
        "AC4",
        prim,
    )
    must("it->second.second != st.ver", "AC4", prim)  # #4792: define_ver_ replaces gen16
    must("auto snap_source_of = [&](const std::string& nm) -> const std::string* {", "AC4", prim)
    must("st.source = aura::ast::unparse_to_string(", "AC4", prim)
    must("++ev.ast_restore_stamp_unparse_total_;", "AC4", prim)
    must("st.has_source = true;", "AC4", prim)
    must("[[nodiscard]] std::uint64_t ast_restore_stamp_unparse_total() const noexcept", "AC4", evixx)
    must("std::uint64_t ast_restore_stamp_republish_total_{0};", "AC4", evixx)

    # AC5 #4360/#4349 machinery still driven by the diff
    must("ev.drop_ir_define_binding_fn_", "AC5", prim)
    must("ev.mark_define_dirty_fn_", "AC5", prim)
    must("constexpr std::size_t kRestoreDiffCap = 48;", "AC5", prim)
    must("MarkAllClearsIrOwners clear_ir_owners;", "AC5", prim)
    must("ev.get_dependents_fn_", "AC5", prim)

    # AC6 stats keys
    must('insert_kv("ast-snapshot-share-hits",', "AC6", obs)
    must('insert_kv("ast-snapshot-share-misses",', "AC6", obs)
    must('insert_kv("ast-restore-stamp-republish",', "AC6", obs)
    must('insert_kv("ast-restore-stamp-unparse",', "AC6", obs)
    must("query_hash_capacity_for(30)", "AC6", obs)

    # AC7 runtime ACs in the existing hosts
    must("--- #4598 AC11: note_subtree_content_write bumps owning define cell ---", "AC7", sym_test)
    must("flat.mark_dirty_upward(fb, FlatAST::kGeneralDirty);", "AC7", sym_test)
    must("ac4598_phase3_snapshot_share_and_stamp_restore", "AC7", round_test)
    must("4598p3 AC11: post-restore snapshot shares the frozen copy", "AC7", round_test)
    must(
        "4598p3 AC12: one-define restore unparses O(dirty cone), not O(workspace)",
        "AC7",
        round_test,
    )
    must(
        "4598p3 AC13: changed-workspace snapshot does not share a stale copy",
        "AC7",
        round_test,
    )
    must_not("test_issue_4598.cpp", "AC7", round_test)

    # AC9 #4271 cap counts DISTINCT frozen copies (#4598 phase 4 residual).
    # Phase 3 aliases one frozen copy across share-hit entries; entry-counting
    # evicted an aliased copy (frees nothing) and demoted the affected ids to
    # the O(workspace) source reparse (7.3 ms vs 0.18 ms at W=200).
    must("Issue #4271 / #4598 residual / #4772: count DISTINCT images", "AC9", prim)
    must("auto [w, distinct] = totals();", "AC9", prim)
    must("if (s.has_flat && s.flat.get() == oldest) {", "AC9", prim)
    must("std::unordered_map<const aura::ast::FlatAST::Image*, char> seen;", "AC9", prim)
    must("4598p4 AC14: distinct-copy count stays 1", "AC9", round_test)
    must("4598p4 AC14: shared burst does not evict the live copy", "AC9", round_test)

    # AC10 eval-current IO replay probe is SoA-only: O(nodes) scan without a
    # NodeView assemble per live node (callgrind: 698k assembles / 20 swaps).
    must("Issue #4598 residual: SoA-only probe.", "AC10", prim_eval)
    must("if (f.tag(id) != aura::ast::NodeTag::Call)", "AC10", prim_eval)
    must("const auto ch = f.children(id);", "AC10", prim_eval)
    must_not("auto node = f.get(id);", "AC10", prim_eval)

    # AC11 wiring + allowlist
    must("check_snapshot_share_stamps_4598.py", "AC11", build_py)
    must("check_snapshot_share_stamps_4598.py", "AC11", allowlist)

    if fails:
        for f in fails:
            print(f"FAIL check_snapshot_share_stamps_4598: {f}")
        return 1
    print("check_snapshot_share_stamps_4598: OK (11 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
