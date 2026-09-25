#!/usr/bin/env python3
"""Issue #4090 source-cite gate: shape hot paths — no per-result flush/heap/atomic.

record_eval_result_shape does record_shape(fn) && is_stable(fn) after every
IR interpret / JIT result. From the second sample of a stable function
onward every result:

  1. record_shape fetch_add'ed hotpath_invariant_hits_total before the TLS
     fast path (process-wide atomic, not sampled).
  2. is_stable flushed the TLS slot under the shard unique_lock and
     FnProfile::compute_dominant built an unordered_map over the history
     window while the lock was held.
  3. Then took the shared lock for the read the flush just computed.

Fix keeps the shard map + TLS slots: the hot reads answer from the shard
when the pending TLS slot provably cannot flip the answer, compute_dominant
counts the closed inline ShapeID set in a stack array, and record_shape's
hotpath tick is the sampled thread-local variant.

ACs:
  AC1  record_shape uses the sampled hotpath tick (the unsampled per-call
       fetch_add is gone from its body) and keeps the tls_record_ merge
       path (shard map + TLS slots stay).
  AC2  is_stable / dominant_shape / current_snapshot answer via the
       pre-flush tls_pending_stable_read_ fast path (flush path preserved).
  AC3  FnProfile::compute_dominant counts in a stack array — no
       unordered_map under the shard lock; the history walk stays.
  AC4  the design marker (kShapeHotReadNoFlushIssue) lives in the header,
       test_shape drives the four runtime ACs, and the linter is wired in
       build.py + scripts/coverage/root_check_allowlist.txt.
  AC5  no new query key and no stray ship files (no
       tests/**/test_issue_4090.cpp, no docs/design/4090-*).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / "src" / "compiler" / "shape_profiler.cpp"
HPP = ROOT / "src" / "compiler" / "shape_profiler.h"
TST = ROOT / "tests" / "compiler" / "test_shape.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def function_body(src: str, signature: str) -> str:
    """Return the text from `signature` up to the next top-level closing brace."""
    i = src.find(signature)
    if i < 0:
        return ""
    j = src.find("\n}", i)
    return src[i : j if j >= 0 else len(src)]


def main() -> int:
    cpp = CPP.read_text() if CPP.exists() else ""
    hpp = HPP.read_text() if HPP.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1: record_shape's per-call hotpath tick is the sampled variant —
    # the unsampled fetch_add is gone from its body — while the TLS
    # record/merge path (tls_record_) stays.
    body = function_body(cpp, "bool ShapeProfiler::record_shape(FnKey fn, ShapeID shape_id) {")
    report(
        "AC1 record_shape sampled tick + TLS merge kept",
        bool(body)
        and "record_hotpath_invariant_hit_sampled()" in body
        and "record_hotpath_invariant_hit()" not in body
        and "tls_record_" in body,
        "record_shape bumps via record_hotpath_invariant_hit_sampled() and keeps tls_record_",
    )

    # AC2: the three hot reads answer via the pre-flush pending-slot fast
    # path; the flush path is preserved behind it (no new query key, no
    # counter inserted in a metrics struct — the reads keep their contract).
    for fn_name, sig in (
        ("is_stable", "bool ShapeProfiler::is_stable(FnKey fn) const {"),
        ("dominant_shape", "ShapeID ShapeProfiler::dominant_shape(FnKey fn) const {"),
        ("current_snapshot", "ShapeSnapshot ShapeProfiler::current_snapshot(FnKey fn) const {"),
    ):
        body = function_body(cpp, sig)
        report(
            f"AC2 {fn_name} no-flush fast path",
            bool(body)
            and "tls_pending_stable_read_" in body
            and body.find("tls_pending_stable_read_") < body.find("flush_tls_records"),
            "answers from the shard before any flush; flush fallback preserved",
        )

    # AC3: compute_dominant counts the closed inline ShapeID set in a stack
    # array — no unordered_map (heap) under the shard unique_lock.
    body = function_body(cpp, "ShapeID ShapeProfiler::FnProfile::compute_dominant() const {")
    report(
        "AC3 compute_dominant heap-free",
        bool(body) and "std::unordered_map" not in body and "counts[" in body and "history.for_each" in body,
        "stack-array bucket counts; no unordered_map; history walk kept",
    )

    # AC4: design marker in the header, runtime ACs in the hosting test,
    # and the linter wired (build.py registration + allowlist row).
    report(
        "AC4 header marker + test ACs + wiring",
        "kShapeHotReadNoFlushIssue = 4090" in hpp
        and "#4090 AC1" in tst
        and "#4090 AC2" in tst
        and "#4090 AC3" in tst
        and "#4090 AC4" in tst
        and "check_shape_hotpath_4090.py" in build
        and "check_shape_hotpath_4090.py" in allow,
        "header issue constant, test_shape ACs 1-4, build.py + allowlist wiring",
    )

    # AC5: negative contracts from the issue — no new query key, no stray
    # ship files.
    stray_test = list(ROOT.glob("tests/**/test_issue_4090.cpp"))
    stray_doc = list(ROOT.glob("docs/design/4090-*"))
    report(
        "AC5 no new query key / no stray files",
        "query:shape-hot" not in (cpp + hpp) and not stray_test and not stray_doc,
        "no query:shape-hot* key; no test_issue_4090.cpp; no docs/design/4090-*",
    )

    print("check_shape_hotpath_4090: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
