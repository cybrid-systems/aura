#!/usr/bin/env python3
# scripts/check_expand_all_deny_codes_3651.py -- Issue #3651 source-cite gate.
#
# AC1: the multi-pass macro_expand_all_body !expanded_any guard refuses the
#      residual half-tree for EVERY inner expand deny code via
#      inner_expand_production_limit_deny() (depth/pass/steal/cap/gensym);
#      the old reason==2 depth-only check is gone; cites #3651.
# AC2: the predicate covers GensymCeiling / StealAbort / CapabilityDeny
#      (single-clone #3183 codes stay guarded — no regression).
# AC3: the #3062 pass-limit loop-end belt is untouched —
#      note_hygiene_last_limit_reason(kHygieneLimitReasonPassLimit) +
#      production try_restore + original_root remain after the pass loop.
# AC4: production_surface gate kept; the Soft half-write path
#      (restamp_after_expand + return root) remains the non-production
#      behavior; #3183 single-clone try_restore sites untouched.
# AC5: test wiring — ac3651_1..5 in tests/compiler/
#      test_macro_hygiene_limits.cpp with the gensym two-macro tree; no
#      tests/**/test_issue_3651.cpp; no docs/design/3651*; build.py
#      registration + root_check_allowlist append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ME = "src/compiler/macro_expansion.cpp"
TEST = "tests/compiler/test_macro_hygiene_limits.cpp"
BUILD = "build.py"

LINTER = "check_expand_all_deny_codes_3651"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(me: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — widened guard replaces the depth-only check.
    must("Issue #3651: the deny codes are wider than depth-limit", "AC1 cite", me)
    must("production_surface && any_expand && inner_expand_production_limit_deny()", "AC1 widened guard", me)
    guard_pos = me.find("production_surface && any_expand && inner_expand_production_limit_deny()")
    old_pos = me.find("g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed) == 2")
    if guard_pos < 0:
        fails.append("AC1: widened guard not located")
    if old_pos >= 0:
        fails.append("AC1: old depth-only (reason==2) check still present")
    must("expand_ckpt.try_restore();", "AC1 restore", me[guard_pos : guard_pos + 400])
    must("return original_root;", "AC1 return original_root", me[guard_pos : guard_pos + 400])

    # AC2 — predicate covers every deny code.
    # Issue #3684: the helper is exported (de-static) and ORs 8/9/10.
    pred_begin = me.find("bool inner_expand_production_limit_deny()")
    pred_end = me.find("namespace detail {", pred_begin)
    if pred_begin < 0 or pred_end < 0 or pred_end <= pred_begin:
        fails.append("AC2: predicate body not located")
    else:
        pred = me[pred_begin:pred_end]
        for code in (
            "kHygieneLimitReasonDepthLimit",
            "kHygieneLimitReasonPassLimit",
            "kHygieneLimitReasonStealAbort",
            "kHygieneLimitReasonCapabilityDeny",
            "kHygieneLimitReasonGensymCeiling",
        ):
            must(code, "AC2 predicate code", pred)

    # AC3 — #3062 pass-limit loop-end belt untouched.
    must("note_hygiene_last_limit_reason(kHygieneLimitReasonPassLimit)", "AC3 belt stamp", me)
    belt_pos = me.find("note_hygiene_last_limit_reason(kHygieneLimitReasonPassLimit)")
    tail = me[belt_pos : belt_pos + 1200]
    must("expand_ckpt.try_restore();", "AC3 belt restore", tail)
    must("return original_root;", "AC3 belt original_root", tail)

    # AC4 — production gate + Soft half-write path intact; single-clone sites.
    if guard_pos >= 0:
        # The single-clone consult sites (clone walk / restamp / expand_inner)
        # still call the predicate (#3183 no regression) — guard adds one more.
        count = me.count("inner_expand_production_limit_deny()")
        if count < 4:
            fails.append(f"AC4: expected >=4 predicate consults (guard + 3 single-clone), got {count}")
        must("if (any_expand)", "AC4 soft half-write branch", me[guard_pos : guard_pos + 400])
        must("restamp_after_expand(flat);", "AC4 soft restamp", me[guard_pos : guard_pos + 400])
        must("return root;", "AC4 soft return root", me[guard_pos : guard_pos + 400])

    # AC5 — test wiring + registration + forbidden artifacts.
    must("static void fill_gensym_two_pass_macros(FlatAST& flat, StringPool& pool) {", "AC5 gensym tree", test)
    must("aura_test_set_max_gensym_map_size_for_test(2);", "AC5 gensym knob", test)
    for ac in (
        "static void ac3651_1_gensym_ceiling_restore()",
        "static void ac3651_2_deny_codes_widened_predicate()",
        "static void ac3651_3_depth_pass_still_restore()",
        "static void ac3651_4_soft_half_write()",
        "static void ac3651_5_source_wiring()",
    ):
        must(ac, "AC5 runner wired", test)
    must("ac3651_1_gensym_ceiling_restore();", "AC5 main call", test)
    must('"hygiene-gensym-ceiling"', "AC5 reason string pinned", test)
    must_not("test_issue_3651", "AC5 no tests/issues literal", test)
    must("check_expand_all_deny_codes_3651", "AC5 build.py registration", build)
    for stale in ROOT.glob("docs/design/*3651*"):
        fails.append(f"AC5: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_3651*.cpp"):
        fails.append(f"AC5: forbidden issue test {stale.name}")

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #3651 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    me = _read(ME)
    test = _read(TEST)
    build = _read(BUILD)
    if args.self_test:
        broken = me.replace("Issue #3651", "Issue #redacted")
        self_fails = _rows(broken, test, build)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(me, test, build)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
