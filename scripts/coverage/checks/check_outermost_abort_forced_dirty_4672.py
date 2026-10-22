#!/usr/bin/env python3
"""Issue #4672: the outermost abort must force-dirty the coerced nodes.

#3102's abort rewind truncates the type cone back to the boundary-entry
size (AC1) and force-dirties the nodes that were coercion-eliminated
inside the aborted window (AC2).  Until #4672 the AC2 step was a NO-OP at
the OUTERMOST boundary: Evaluator::exit_mutation_boundary() called
coerced_nodes_tracker_exit_boundary() -- which CLEARS the TLS tracker when
the depth reaches 0 -- BEFORE the abort site took it, so the take always
returned empty and the coerced nodes kept their stale coercion elision.

The fix captures the tracker BEFORE the exit call drains it and feeds the
capture to the abort rewind.  This gate is the static half of that
contract; it checks ORDER (not substring presence), so the regression
cannot come back through a reshuffled comment:

  AC1  the capture statement runs BEFORE the tracker drain, inside
       Evaluator::exit_mutation_boundary (brace-matched function body).
  AC2  the #3102 abort rewind block consumes the capture
       (std::move(coerced_nodes_at_exit)) and still runs AC1 (cone
       truncate) / AC3 (decision invalidate) / AC4 (commit_readiness
       clear) plus the force-dirty step.
  AC3  the capture is gated on !success -- the success path still
       discards the vector exactly as before.
  AC4  the Soft / Off observe branch drops the capture: no forced-dirty
       and no counter bump on the soft path.
  AC5  the other two #3102 abort sites are untouched: they still take the
       tracker themselves, and the tracker drain is still called once.

Usage:
  python3 scripts/coverage/checks/check_outermost_abort_forced_dirty_4672.py
  python3 scripts/coverage/checks/check_outermost_abort_forced_dirty_4672.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TARGET_REL = "src/compiler/evaluator_mutation_boundary.cpp"

FUNC_SIG = "Evaluator::MutationCheckpoint Evaluator::exit_mutation_boundary("
REWIND_ANCHOR = "// Issue #3102: AC1+AC2+AC3+AC4 — CoercionMap abort rewind."

CAPTURE_RE = re.compile(r"coerced_nodes_at_exit\s*=\s*aura::compiler::coerced_nodes_tracker_take\(\)\s*;")
DRAIN_RE = re.compile(r"aura::compiler::coerced_nodes_tracker_exit_boundary\(\)\s*;")
CAPTURE_DECL_RE = re.compile(r"std::vector<aura::compiler::dirty::NodeId>\s+coerced_nodes_at_exit\s*;")
NOT_SUCCESS_RE = re.compile(r"if\s*\(\s*!success\s*\)")
CONSUME_RE = re.compile(r"std::move\(coerced_nodes_at_exit\)")
CLEAR_RE = re.compile(r"coerced_nodes_at_exit\.clear\(\)\s*;")
OBSERVE_RE = re.compile(r"g_coercion_map_abort_rewind_observe_total")
FORCE_DIRTY_RE = re.compile(r"force_dead_coercion_elim_into_cone")
DIRECT_TAKE_RE = re.compile(r"auto coerced = aura::compiler::coerced_nodes_tracker_take\(\)\s*;")

UNTOUCHED_SITE_ANCHORS = (
    "// Issue #3030: invariant force-rollback clears proof face.",
    "// Issue #3030: Strict reflect-validate rollback clears proof face.",
)

REWIND_REQUIRED = (
    "truncate_type_cone_to_size(cp.coercion_cone_size_at_entry)",
    "force_dead_coercion_elim_into_cone(coerced)",
    "g_coercion_map_abort_forced_dirty_total",
    "bump_dead_coercion_decision_invalidate",
    "clear_coercion_commit_readiness_on_abort",
)


def _brace_block(text: str, from_idx: int) -> str:
    """Brace-matched block that starts at the first '{' at/after from_idx.

    String / char literals and comments are skipped so a brace inside them
    does not unbalance the block.
    """
    open_at = text.find("{", from_idx)
    if open_at < 0:
        return ""
    i = open_at
    n = len(text)
    depth = 0
    in_str = False
    in_chr = False
    in_line = False
    in_blk = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else "\0"
        if in_line:
            if c == "\n":
                in_line = False
            i += 1
            continue
        if in_blk:
            if c == "*" and nxt == "/":
                in_blk = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_chr:
            if c == "\\":
                i += 2
                continue
            if c == "'":
                in_chr = False
            i += 1
            continue
        if c == "/" and nxt == "/":
            in_line = True
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_blk = True
            i += 2
            continue
        if c == '"':
            in_str = True
            i += 1
            continue
        if c == "'":
            in_chr = True
            i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_at : i + 1]
        i += 1
    return text[open_at:]


def check_text(text: str) -> list[str]:
    """Return the failure list (empty = OK)."""
    fails: list[str] = []

    sig_at = text.find(FUNC_SIG)
    if sig_at < 0:
        return [f"AC1: {FUNC_SIG!r} not found in the boundary source"]
    body = _brace_block(text, sig_at)
    if not body:
        return ["AC1: the exit_mutation_boundary body could not be delimited"]

    # -- AC1: the capture runs BEFORE the drain ------------------------------
    cap = CAPTURE_RE.search(body)
    drain = DRAIN_RE.search(body)
    if cap is None:
        fails.append(
            "AC1: the coerced-node capture (coerced_nodes_at_exit = "
            "coerced_nodes_tracker_take()) is missing from exit_mutation_boundary"
        )
    if drain is None:
        fails.append("AC1: the tracker drain (coerced_nodes_tracker_exit_boundary) is missing")
    if cap is not None and drain is not None and cap.start() > drain.start():
        fails.append(
            "AC1: the capture runs AFTER the tracker drain -- the drain "
            "clears the vector at depth 0, so the abort rewind would "
            "still take an empty tracker"
        )

    # -- AC3: the capture is gated on !success -------------------------------
    if cap is not None:
        head = body[max(0, cap.start() - 400) : cap.start()]
        if NOT_SUCCESS_RE.search(head) is None:
            fails.append(
                "AC3: the capture is not gated on !success -- the success path "
                "must still discard the tracker exactly as before"
            )
        if CAPTURE_DECL_RE.search(body) is None:
            fails.append("AC3: the captured vector declaration is missing")

    # -- AC2: the abort rewind consumes the capture and keeps its own ACs ----
    anchor = body.find(REWIND_ANCHOR)
    if anchor < 0:
        fails.append(f"AC2: the abort rewind anchor {REWIND_ANCHOR!r} is missing")
    else:
        win = body[anchor : anchor + 2000]
        if CONSUME_RE.search(win) is None:
            fails.append(
                "AC2: the abort rewind does not consume the captured vector "
                "(std::move(coerced_nodes_at_exit)) -- it would take an "
                "already-drained tracker"
            )
        for needle in REWIND_REQUIRED:
            if needle not in win:
                fails.append(f"AC2: the abort rewind lost {needle!r}")

    # -- AC4: the Soft / Off observe branch drops the capture ----------------
    obs = OBSERVE_RE.search(body)
    if obs is None:
        fails.append("AC4: the Soft / Off observe branch is missing")
    else:
        nxt = FORCE_DIRTY_RE.search(body, obs.end())
        seg = body[obs.end() : nxt.start() if nxt else len(body)]
        if FORCE_DIRTY_RE.search(seg) is not None:
            fails.append("AC4: the Soft / Off observe branch force-dirties nodes")
        if CLEAR_RE.search(seg) is None:
            fails.append(
                "AC4: the Soft / Off observe branch does not drop the capture -- Soft / Off must stay observe-only"
            )

    # -- AC5: the other two #3102 abort sites are untouched ------------------
    for anchor_txt in UNTOUCHED_SITE_ANCHORS:
        at = text.find(anchor_txt)
        if at < 0:
            fails.append(f"AC5: the untouched #3102 abort site {anchor_txt!r} is missing")
            continue
        win = text[at : at + 1500]
        if DIRECT_TAKE_RE.search(win) is None:
            fails.append(
                f"AC5: the #3102 abort site {anchor_txt!r} no longer takes the "
                "tracker itself -- that site is out of #4672's scope"
            )
    drains = DRAIN_RE.findall(text)
    if len(drains) != 1:
        fails.append(f"AC5: expected exactly 1 tracker drain call, found {len(drains)}")

    return fails


def _synthetic(*, capture_after: bool = False, direct_take: bool = False, soft_dirty: bool = False) -> str:
    """Build a synthetic boundary source (self-test fixture)."""
    capture = "    if (!success)\n        coerced_nodes_at_exit = aura::compiler::coerced_nodes_tracker_take();\n"
    drain = "    aura::compiler::coerced_nodes_tracker_exit_boundary();\n"
    if capture_after:
        capture, drain = drain, capture
    take_line = (
        "            auto coerced = aura::compiler::coerced_nodes_tracker_take();\n"
        if direct_take
        else "            auto coerced = std::move(coerced_nodes_at_exit);\n"
    )
    soft = (
        "            aura::compiler::dirty::force_dead_coercion_elim_into_cone(coerced);\n"
        if soft_dirty
        else "            coerced_nodes_at_exit.clear();\n"
    )
    body = (
        "Evaluator::MutationCheckpoint Evaluator::exit_mutation_boundary(bool success) {\n"
        "    std::vector<aura::compiler::dirty::NodeId> coerced_nodes_at_exit;\n"
        + capture
        + drain
        + "    if (typed_audit::production_defaults_active()) {\n"
        + "        // Issue #3102: AC1+AC2+AC3+AC4 — CoercionMap abort rewind.\n"
        + "        aura::compiler::dirty::truncate_type_cone_to_size(cp.coercion_cone_size_at_entry);\n"
        + take_line
        + "        if (!coerced.empty()) {\n"
        + "            aura::compiler::dirty::force_dead_coercion_elim_into_cone(coerced);\n"
        + "            aura::compiler::g_coercion_map_abort_forced_dirty_total.fetch_add(1, 0);\n"
        + "        }\n"
        + "        aura::compiler::dirty::bump_dead_coercion_decision_invalidate();\n"
        + "        typed_audit::clear_coercion_commit_readiness_on_abort();\n"
        + "    } else {\n"
        + "        aura::compiler::g_coercion_map_abort_rewind_observe_total.fetch_add(1, 0);\n"
        + soft
        + "        (void)aura::compiler::coerced_nodes_tracker_take();\n"
        + "    }\n"
        + "}\n"
        + "void other_site_a() {\n"
        + "    // Issue #3030: invariant force-rollback clears proof face.\n"
        + "    auto coerced = aura::compiler::coerced_nodes_tracker_take();\n"
        + "    (void)coerced;\n"
        + "}\n"
        + "void other_site_b() {\n"
        + "    // Issue #3030: Strict reflect-validate rollback clears proof face.\n"
        + "    auto coerced = aura::compiler::coerced_nodes_tracker_take();\n"
        + "    (void)coerced;\n"
        + "}\n"
    )
    return body


def _self_test() -> int:
    fails = check_text(_synthetic())
    if fails:
        print("FAIL #4672 self-test: the well-formed source should pass:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1

    cases = (
        ("capture after the drain", _synthetic(capture_after=True), "AC1"),
        ("rewind takes the drained tracker", _synthetic(direct_take=True), "AC2"),
        ("soft branch force-dirties", _synthetic(soft_dirty=True), "AC4"),
    )
    for label, text, ac in cases:
        got = check_text(text)
        if not any(f.startswith(ac) for f in got):
            print(f"FAIL #4672 self-test: the {label} was not caught ({ac})", file=sys.stderr)
            for f in got:
                print(f"  - {f}", file=sys.stderr)
            return 1

    print("OK #4672 self-test: the capture/drain ordering gate catches the regression")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Issue #4672 outermost abort force-dirty gate")
    ap.add_argument("--self-test", action="store_true", help="run the built-in probes")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    target = ROOT / TARGET_REL
    if not target.is_file():
        print(f"FAIL #4672 outermost_abort_forced_dirty: missing {target}", file=sys.stderr)
        return 1
    text = target.read_text(encoding="utf-8", errors="replace")
    fails = check_text(text)
    if fails:
        print(f"FAIL #4672 outermost_abort_forced_dirty: {target}", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("OK #4672 outermost_abort_forced_dirty: the capture precedes the drain and the abort rewind consumes it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
