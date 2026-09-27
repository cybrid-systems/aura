#!/usr/bin/env python3
# scripts/check_lockless_macro_allow_latch_4127.py -- Issue #4127 gate.
#
# AC1: lockless :replace-subtree — the MacroIntroduced allow arm notes the
#      #3637 boundary allow latch AFTER the MSE gate passes (#4035 public
#      parity; Soft/Off stays latch-free) and the install site propagates
#      MacroIntroduced onto the replacement through the SHARED #2858 cascade
#      when target_was_macro, honoring :no-auto-restamp? (public #3061 arm
#      parity) — propagate ordered after add_mutation_subtree.
# AC2: lockless :set-body — same latch on the MSE success path (define/lambda
#      arm) and the same install-site propagate when the target/lambda was MI
#      or the new body carries MI (public mutate:set-body parity: default
#      restamp, no :no-auto-restamp? on this prim).
# AC3: lockless :tweak-literal latches on the MSE success path (public
#      parity: hygiene_protected_error latches; set_int keeps the marker —
#      no propagate on the in-place tweak); lockless :move-node latches on
#      BOTH MSE arms (moved node + dest parent) and propagates when
#      was_macro_mv || parent_was_macro_mv with :no-auto-restamp?.
# AC4: ONE cascade — evaluator.ixx declares primitives_detail::
#      propagate_macro_introduced_marker (default opt_out=false) and
#      parse_no_auto_restamp_opt_out; the definitions are de-static'd
#      (external linkage) in evaluator_primitives_mutate.cpp;
#      evaluator_eval_flat.cpp has NO file-local mirror of either and no
#      duplicated restamp counters (no second marker channel).
# AC5: #3637 net authority untouched (get_allow_macro_mutate() ||
#      boundary_macro_allow_latched()); every latch sits INSIDE an
#      effect_sandbox_mode() != 0 block (Soft/Off early-out preserved);
#      public-path parity lines intact; build.py + root_check_allowlist
#      wiring present; no docs/design/4127-*; no tests/**/test_issue_4127*.

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

EFL = "src/compiler/evaluator_eval_flat.cpp"
MUT = "src/compiler/evaluator_primitives_mutate.cpp"
IXX = "src/compiler/evaluator.ixx"
NET = "src/compiler/evaluator_mutation_boundary.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

LATCH = "note_boundary_macro_allow_latch();"
PROP = "primitives_detail::propagate_macro_introduced_marker("
NOSTAMP = "primitives_detail::parse_no_auto_restamp_opt_out(*this, a)"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _win(src: str, fn: str) -> str:
    """Slice one eval_flat_apply_mutate_<fn> function body (next-def bound)."""
    key = f"EvalResult Evaluator::eval_flat_apply_mutate_{fn}("
    pos = src.find(key)
    if pos == -1:
        return ""
    nxt = src.find("EvalResult Evaluator::eval_flat_apply_mutate_", pos + len(key))
    end = nxt if nxt != -1 else pos + 12000
    return src[pos:end]


def _count(hay: str, needle: str) -> int:
    n = 0
    pos = 0
    while True:
        pos = hay.find(needle, pos)
        if pos == -1:
            return n
        n += 1
        pos += len(needle)


def _rows() -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    efl = _read(EFL)
    mut = _read(MUT)
    ixx = _read(IXX)
    net = _read(NET)
    build = _read(BUILD)
    allow = _read(ALLOWLIST)
    if not efl or not mut or not ixx or not net:
        fails.append("inputs: source files unreadable")
        return fails

    # ── AC1: lockless :replace-subtree latch + shared-cascade propagate ──
    rs = _win(efl, "replace_subtree")
    if not rs:
        fails.append("AC1: eval_flat_apply_mutate_replace_subtree window not found")
    else:
        must("Issue #4127", "AC1", rs)
        must("const bool target_was_macro = flat.is_macro_introduced(target);", "AC1", rs)
        must("if (target_was_macro) {", "AC1", rs)
        must(LATCH, "AC1", rs)
        must(PROP, "AC1", rs)
        must(NOSTAMP, "AC1", rs)
        if (
            LATCH in rs
            and "deny_macro_opt_out_without_mse(*this, target)" in rs
            and rs.find("deny_macro_opt_out_without_mse(*this, target)") > rs.find(LATCH)
        ):
            fails.append("AC1: latch must sit after the target MSE gate")
        if PROP in rs and "add_mutation_subtree(" in rs and rs.find(PROP) < rs.find("add_mutation_subtree("):
            fails.append("AC1: propagate must sit after the install (add_mutation_subtree)")

    # ── AC2: lockless :set-body latch + propagate ──
    sb = _win(efl, "set_body")
    if not sb:
        fails.append("AC2: eval_flat_apply_mutate_set_body window not found")
    else:
        must("Issue #4127", "AC2", sb)
        must(
            "flat.is_macro_introduced(target) || flat.is_macro_introduced(lambda_id);",
            "AC2",
            sb,
        )
        must(LATCH, "AC2", sb)
        must("if (target_was_macro || body_hit_macro)", "AC2", sb)
        must(PROP + "*this, flat, body_to_set);", "AC2", sb)
        must_not("body_to_set, primitives_detail::parse_no_auto_restamp_opt_out", "AC2", sb)

    # ── AC3: tweak-literal latch-only; move-node double latch + propagate ──
    tl = _win(efl, "tweak_literal")
    if not tl:
        fails.append("AC3: eval_flat_apply_mutate_tweak_literal window not found")
    else:
        must(LATCH, "AC3", tl)
        must_not(PROP, "AC3 (public parity: no propagate on in-place tweak)", tl)
    mv = _win(efl, "move_node")
    if not mv:
        fails.append("AC3: eval_flat_apply_mutate_move_node window not found")
    else:
        must("const bool was_macro_mv = flat.is_macro_introduced(node);", "AC3", mv)
        must("const bool parent_was_macro_mv = flat.is_macro_introduced(new_parent);", "AC3", mv)
        if _count(mv, LATCH) < 2:
            fails.append("AC3: move-node must latch on BOTH MSE arms (node + dest parent)")
        must("if (was_macro_mv || parent_was_macro_mv)", "AC3", mv)
        must(PROP, "AC3", mv)
        must(NOSTAMP, "AC3", mv)

    # ── AC4: ONE cascade — module-linked declaration + de-static + no mirror ──
    must("Issue #4127", "AC4", ixx)
    must(
        "void propagate_macro_introduced_marker(Evaluator& ev, aura::ast::FlatAST& flat,",
        "AC4",
        ixx,
    )
    must("aura::ast::NodeId new_root, bool opt_out = false);", "AC4", ixx)
    must(
        "bool parse_no_auto_restamp_opt_out(Evaluator& ev, std::span<const types::EvalValue> args);",
        "AC4",
        ixx,
    )
    must(
        "void propagate_macro_introduced_marker(Evaluator& ev, aura::ast::FlatAST& flat,",
        "AC4",
        mut,
    )
    must("aura::ast::NodeId new_root, bool opt_out) {", "AC4", mut)
    must(
        "bool parse_no_auto_restamp_opt_out(Evaluator& ev, std::span<const types::EvalValue> args) {",
        "AC4",
        mut,
    )
    must_not("static void propagate_macro_introduced_marker", "AC4", mut)
    must_not("static bool parse_no_auto_restamp_opt_out", "AC4", mut)
    must_not("static void propagate_macro_introduced_marker", "AC4 (no file-local mirror)", efl)
    must_not("static bool parse_no_auto_restamp_opt_out", "AC4 (no file-local mirror)", efl)
    must_not("macro_mutate_auto_restamp_total", "AC4 (no duplicated restamp counters)", efl)
    if _count(efl, PROP) != 3:
        fails.append(f"AC4: expected 3 shared-cascade call sites in eval_flat, found {_count(efl, PROP)}")

    # ── AC5: net authority untouched + Soft early-out + public parity + wiring ──
    must("ev_->get_allow_macro_mutate() || ev_->boundary_macro_allow_latched()", "AC5", net)
    for name in ("replace_subtree", "set_body", "tweak_literal", "move_node"):
        w = _win(efl, name)
        must("if (effect_sandbox_mode() != 0) {", f"AC5 ({name})", w)
    must("if (allow_macro_rs && target_was_macro)", "AC5 (public parity rs)", mut)
    must("if (allow_macro_set_body && was_macro_set_body)", "AC5 (public parity sb)", mut)
    must(
        "if (allow_macro_mv && (was_macro_mv || parent_was_macro_mv))",
        "AC5 (public parity mv)",
        mut,
    )
    must("check_lockless_macro_allow_latch_4127.py", "AC5 (build.py registration)", build)
    must("check_lockless_macro_allow_latch_4127.py", "AC5 (root_check_allowlist)", allow)
    for artifact in (
        "docs/design/4127-lockless-macro-allow-latch.md",
        "tests/compiler/test_issue_4127.cpp",
        "tests/issues/test_issue_4127.cpp",
    ):
        if (ROOT / artifact).is_file():
            fails.append(f"AC5: forbidden artifact present: {artifact}")
    return fails


def main() -> int:
    fails = _rows()
    if fails:
        for f in fails:
            print(f"FAIL check_lockless_macro_allow_latch_4127: {f}")
        print(f"check_lockless_macro_allow_latch_4127: {len(fails)} failure(s)")
        return 1
    print("check_lockless_macro_allow_latch_4127: OK (Issue #4127 lockless allow-arm latch + shared cascade pinned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
