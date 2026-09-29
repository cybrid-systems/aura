#!/usr/bin/env python3
"""Issue #4169: apply_closure closures-shard shared_lock on TLS miss still
amplifies multi-fiber apply under densify windows (P1).

Hole: the #3832 TLS cache keyed each slot on an exact g_last_window_seq
snapshot, so every Moving densify publish dropped ALL resident slots. The
next apply per fiber then took closures_shards_[shard].mu shared_lock +
(note under production) the #3421 densify refuse consult — a multi-fiber
miss storm on hot ClosureIds whenever densify windows publish often.

Fix shape (existing SSOTs only; no new scheduler, no second densify model):
  - try_apply_closure_tls_lookup drops a slot only on content
    invalidation: closures_apply_epoch_ mismatch (every shard-map
    unique-write / erase bumps it — #3832 face preserved verbatim).
  - A densify window_seq bump re-stamps the resident slot
    (s.densify_window_seq = dseq) instead of dropping it — "seq-stable
    hits survive"; flush TLS only.
  - Correctness nets unchanged on the copied Closure: #4066/#4124
    bind_temporary_moving_live_ptr rewrites densify-old flat/pool through
    last_object_remap_ hit or miss, #3021 closure_apply_use_site_ok
    tombstone check runs on the cached copy, #3421
    production_apply_closure_densify_hard_refuse + #4006 seq-skip arm run
    downstream; g_apply_densify_window_consult_total stays bounded.

Contract (one row per AC):
  AC1  seq-survive stamp + re-stamp present in evaluator_eval_flat.cpp;
       the old `epoch != epoch || densify_window_seq != dseq` drop
       condition is gone from the TLS lookup
  AC2  epoch-owned content drop retained (apply_epoch mismatch drops the
       slot) ahead of the use-site tombstone check on the cached copy
  AC3  densify-old-copy nets retained: bind_temporary_moving_live_ptr,
       production_apply_closure_densify_hard_refuse, #4006
       densify_refuse_seq_skip, g_apply_densify_window_consult_total
  AC4  runtime ACs dispatched in test_apply_closure_envframe_soa.cpp
       (ac4169_seq_stable_survive publish storm: TLS hits survive, no mtx
       storm, consult bounded, epoch refill still fires) + source-cite ACs
       (ac4169_source_cite); the #3832 fixture label re-anchored to the
       re-stamp contract; no tests/**/test_issue_4169.cpp; no
       docs/design/4169-*
  AC5  batch runtime door: the wave-8 (#1978) rename had dropped the
       file's target wiring entirely — test_apply_closure_envframe_soa is
       restored as a test_ir_closure_jit_misc_batch member (CMakeLists.txt
       source line + driver extern + isolate-table dispatch +
       run_<stem> member pattern with #ifndef AURA_ISSUE_BATCH_MEMBER
       standalone main); build.py wiring +
       scripts/coverage/root_check_allowlist.txt entry; the #3832
       regression-guard linter ordering row still holds (TLS lookup
       precedes the closures-shard shared_lock on the apply path)

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

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_apply_closure_envframe_soa.cpp")
    driver = _read("tests/compiler/test_ir_closure_jit_misc_batch.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: seq-survive stamp + re-stamp; old drop condition gone ──────
    must("kApplyClosureTlsSeqSurviveIssue = 4169", "AC1 stamp constant", flat)
    must("Issue #4169", "AC1 issue cite", flat)
    lookup = window(
        flat, "static bool try_apply_closure_tls_lookup", "ev.note_apply_closure_tls_hit()", "AC1 lookup body"
    )
    must("s.densify_window_seq = dseq;", "AC1 resident slot re-stamped", lookup)
    must("if (s.apply_epoch != epoch)", "AC1 epoch-owned drop", lookup)
    absent(
        "if (s.apply_epoch != epoch || s.densify_window_seq != dseq)",
        "AC1 old exact-seq drop condition removed",
        lookup,
    )
    # The re-stamp must precede the use-site check (slot marked current
    # before the tombstone refuse can drop it).
    b_restamp = lookup.find("s.densify_window_seq = dseq;")
    b_use = lookup.find("closure_apply_use_site_ok")
    if b_restamp < 0 or b_use < 0 or b_use < b_restamp:
        fails.append("AC1: re-stamp must precede the use-site tombstone check")

    # ── AC2: #3832 faces preserved verbatim ─────────────────────────────
    must("kApplyClosureTlsCacheIssue = 3832", "AC2 3832 stamp kept", flat)
    must("store_apply_closure_tls", "AC2 store path kept", flat)
    must("densify_window_seq_relaxed()", "AC2 seq reader kept", flat)
    must("#4169 seq-stable hits survive", "AC2 evaluator.ixx cite", ixx)
    absent("densify also keys TLS\n", "AC2 ix comment re-anchored", ixx)

    # ── AC3: densify-old-copy correctness nets retained ─────────────────
    must("bind_temporary_moving_live_ptr(cl_copy.flat", "AC3 remap bind net", flat)
    must("production_apply_closure_densify_hard_refuse", "AC3 #3421 refuse consult", flat)
    must("densify_refuse_seq_skip", "AC3 #4006 seq-skip arm", flat)
    must("g_apply_densify_window_consult_total", "AC3 consult counter", flat)
    must("closure_apply_use_site_ok", "AC3 tombstone protocol", flat)

    # ── AC4: runtime + source-cite ACs dispatched in the existing test ──
    must("static void ac4169_seq_stable_survive()", "AC4 runtime AC fn", test)
    must("static void ac4169_source_cite()", "AC4 source-cite AC fn", test)
    must("ac4169_seq_stable_survive();", "AC4 runtime dispatch", test)
    must("ac4169_source_cite();", "AC4 source-cite dispatch", test)
    must("publish_last_moving_densify_window(false, true, false,", "AC4 publish storm", test)
    must("4169 AC1: no closures-shard mtx miss storm across seq bumps", "AC4 mtx-storm row", test)
    must("4169 AC2: g_apply_densify_window_consult_total bounded", "AC4 consult bound row", test)
    must("4169 AC3: epoch bump still forces mtx refill", "AC4 epoch refill row", test)
    must("4169: seq mismatch no longer drops the TLS slot", "AC4 drop-gone row", test)
    must(
        "3832/4169: densify window_seq keys the TLS stamp (bump re-stamps, no drop)",
        "AC4 3832 fixture re-anchored",
        test,
    )
    absent('"3832: densify window_seq invalidates TLS"', "AC4 stale 3832 label removed", test)
    if (ROOT / "tests" / "compiler" / "test_issue_4169.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_4169.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_4169.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_4169.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4169-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    # ── AC5: batch runtime door + wiring + allowlist + #3832 guard row ──
    must("int run_test_apply_closure_envframe_soa()", "AC5 member run_ entry", test)
    must("#ifndef AURA_ISSUE_BATCH_MEMBER", "AC5 standalone main guard", test)
    must(
        "    tests/compiler/test_apply_closure_envframe_soa.cpp\n",
        "AC5 CMakeLists batch member line",
        cmake,
    )
    must("extern int run_test_apply_closure_envframe_soa();", "AC5 driver extern", driver)
    must(
        '{"test_apply_closure_envframe_soa", run_test_apply_closure_envframe_soa},',
        "AC5 driver isolate-table dispatch",
        driver,
    )
    must("(25 members)", "AC5 driver member count bumped", driver)
    must("check_apply_closure_tls_seq_4169", "AC5 build.py wiring", build)
    must("check_apply_closure_tls_seq_4169.py", "AC5 allowlist entry", allow)
    # The #3832 regression-guard ordering row: TLS lookup precedes the shard
    # shared_lock on the apply path (its linter pins this ordering).
    pos_tls = flat.find("try_apply_closure_tls_lookup(*this, cid, cl_copy, tombstoned)")
    pos_mtx = flat.find(
        "std::shared_lock<std::shared_mutex> rlock(closures_shards_[closures_shard_index(cid)].mu);",
        pos_tls if pos_tls >= 0 else 0,
    )
    if pos_tls < 0 or pos_mtx < 0 or pos_mtx < pos_tls:
        fails.append("AC5: TLS lookup must precede the closures shard shared_lock on apply path")
    g3832 = _read("scripts/coverage/checks/check_apply_closure_tls_cache_3832.py")
    if g3832 and 'must("g_last_window_seq", "AC1 densify invalidate", flat)' not in g3832:
        fails.append("AC5: 3832 guard g_last_window_seq row drifted unexpectedly")

    if fails:
        print("FAIL #4169 apply_closure_tls_seq_survive:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #4169 apply_closure_tls_seq_survive: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
