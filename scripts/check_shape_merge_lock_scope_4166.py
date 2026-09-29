#!/usr/bin/env python3
"""Issue #4166 source-cite gate: ShapeProfiler merge-flush lock scope.

Under multi-round self-modify, disjoint FnKeys are sharded
(kShapeProfilerShardCount=16), but same-FnKey fibers still serialized on
the shard unique_lock: the #3357 TLS merge flush ran TWO O(window)
history.for_each walks (compute_dominant + the dominant-count scan)
while holding unique_lock_shard_, so stormy mutate+eval on one hot FnKey
stalled stability reads / deopt hooks.

Fix keeps FnKey sharding / PerEval storm isolation / compact dirty cone
(#2937/#2617/#3199/#3455 unchanged): the ring maintains a RUNNING shape
histogram (exact multiset of the live window, #4090 bucket layout), and
the TLS (fn, shape, count) triples are stack-snapped before the
unique_lock. Under the lock the merge only pushes counts + flips
stability bits — no history.for_each.

ACs:
  AC1  the ring maintains the running histogram: push increments, wrap
       eviction decrements, clear zeroes, and shrink trims (with the
       count clamp that keeps the ring consistent).
  AC2  record_shape_apply_locked_ (comment-stripped) contains no
       for_each and reads the dominant count via count_of(.
  AC3  compute_dominant scans history.counts with no walk, and
       flush_tls_records stack-snaps (and releases) the TLS triples
       before record_shape_apply_locked_.
  AC4  the design marker (kShapeMergeHistogramIssue = 4166) lives in the
       header and test_shape_profiler_concurrency.cpp drives the
       runtime ACs (ac4166_1/ac4166_2/ac4166_3).
  AC5  the linter is wired in build.py + allowlist and no stray ship
       files exist (no tests/**/test_issue_4166.cpp, no
       docs/design/4166-*, no schema-4166).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / "src" / "compiler" / "shape_profiler.cpp"
HPP = ROOT / "src" / "compiler" / "shape_profiler.h"
TST = ROOT / "tests" / "compiler" / "test_shape_profiler_concurrency.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def strip_line_comments(src: str) -> str:
    out: list[str] = []
    i = 0
    n = len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def function_body(src: str, signature: str) -> str:
    """Return the brace-matched body text starting at `signature`."""
    i = src.find(signature)
    if i < 0:
        return ""
    brace = src.find("{", i)
    if brace < 0:
        return ""
    depth = 0
    end = brace
    for end in range(brace, len(src)):
        if src[end] == "{":
            depth += 1
        elif src[end] == "}":
            depth -= 1
            if depth == 0:
                return src[brace : end + 1]
    return src[brace:]


def main() -> int:
    cpp = CPP.read_text() if CPP.exists() else ""
    hpp = HPP.read_text() if HPP.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    cpp_code = strip_line_comments(cpp)
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1: the ring maintains the running histogram at every mutation site —
    # push (increment + wrap-evict decrement), clear (zero), shrink (trim).
    push_body = function_body(cpp, "void ShapeProfiler::ShapeHistoryRing::push(")
    report(
        "AC1 ring histogram maintained at push/evict/clear/shrink",
        bool(push_body)
        and "note_inc(rec.shape_id)" in push_body
        and "note_dec(slots[head].shape_id)" in push_body
        and "void clear() noexcept" in hpp
        and "void note_inc(ShapeID id) noexcept" in hpp
        and "void note_dec(ShapeID id) noexcept" in hpp
        and "note_dec(slots[i].shape_id)" in hpp
        and "count = cap" in hpp,
        "push inc + wrap dec + clear zero + shrink trim (count clamp)",
    )

    # AC2: the merge critical section is O(n) pushes + O(bucket) scans —
    # no history walk under the shard unique_lock (comment-stripped).
    merge_body = function_body(cpp_code, "bool ShapeProfiler::record_shape_apply_locked_")
    report(
        "AC2 merge critical section walk-free",
        bool(merge_body) and "for_each" not in merge_body and "count_of(" in merge_body,
        "record_shape_apply_locked_ has no for_each; dominant count via count_of(",
    )

    # AC3: compute_dominant reads the running histogram, and the TLS merge
    # flush stack-snaps the triples before taking the shard unique_lock.
    dom_body = function_body(cpp_code, "ShapeProfiler::FnProfile::compute_dominant")
    flush_body = function_body(cpp_code, "void ShapeProfiler::flush_tls_records")
    apply_pos = flush_body.find("record_shape_apply_locked_(")
    zero_pos = flush_body.find("s.count = 0;")
    report(
        "AC3 histogram-backed dominant + stack snap before lock",
        bool(dom_body)
        and "for_each" not in dom_body
        and "history.counts" in dom_body
        and "Snap" in flush_body
        and apply_pos >= 0
        and zero_pos >= 0
        and zero_pos < apply_pos,
        "compute_dominant scans history.counts; flush snaps + releases slots pre-lock",
    )

    # AC4: design marker in the header, runtime ACs in the hosting test.
    report(
        "AC4 header marker + runtime ACs",
        "kShapeMergeHistogramIssue = 4166" in hpp
        and "kHistogramBuckets" in hpp
        and "ac4166_1_wrap_eviction_histogram_exact" in tst
        and "ac4166_2_same_fnkey_merge_under_fibers" in tst
        and "ac4166_3_window_shrink_trims_histogram" in tst,
        "kShapeMergeHistogramIssue in header; ac4166_1..3 in the concurrency suite",
    )

    # AC5: wiring + negative contracts — no stray ship files, prior shape
    # gates (#4090 walk-free hot reads, #2617 compact≠storm) untouched.
    stray_test = list(ROOT.glob("tests/**/test_issue_4166.cpp"))
    stray_doc = list(ROOT.glob("docs/design/4166-*"))
    report(
        "AC5 wiring + no stray files",
        "check_shape_merge_lock_scope_4166.py" in build
        and "check_shape_merge_lock_scope_4166.py" in allow
        and "check_shape_hotpath_4090.py" in build
        and not stray_test
        and not stray_doc
        and "schema-4166" not in (cpp + hpp),
        "build.py + allowlist rows; #4090 gate kept; no test_issue_4166/docs/schema",
    )

    print("check_shape_merge_lock_scope_4166: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
