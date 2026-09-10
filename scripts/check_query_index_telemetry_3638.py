#!/usr/bin/env python3
# scripts/check_query_index_telemetry_3638.py -- Issue #3638 source-cite gate.
#
# Verifies per-query-call index hit vs full-scan fallback telemetry:
#   hit      = the (tag,arity) index / defuse index served the query
#   fallback = the probe had to full-rebuild / full-scan (cold, size change,
#              build-from-scratch)
# Counters are exported inline atomics (evaluator.ixx, #2906-style additive);
# bumps live at the two tag/arity snapshot probes + the three defuse
# ensure_defuse branches; the stats faces query:index-hit-total /
# query:full-scan-fallback-total expose them (additive — no schema break).
#
#  AC1: per-query-call counters (relaxed add-only, never per node; no
#       mid-metrics inserts, no query-result schema change).
#  AC2: suite face — cold probe = fallback, synced probe = hit, stats
#       faces readable.
#  AC3: shared-lock scope — the bucket probe holds the index lock across
#       build + by-value bucket copy only (lock not held across match
#       materialization).
#  AC4: stats faces expose both counters.
#  AC6: no docs/design/3638-* (#1655); no tests/**/test_issue_3638.cpp
#       (#81934); build.py wires this linter.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

EVAL_IXX = "src/compiler/evaluator.ixx"
QI = "src/compiler/evaluator_query_index.cpp"
DEFUSE = "src/compiler/evaluator_defuse_index.cpp"
QPRIMS = "src/compiler/evaluator_primitives_query.cpp"
TEST = "tests/compiler/test_query_by_marker_provenance.cpp"

REQUIRED: tuple[tuple[str, str, str], ...] = (
    (
        EVAL_IXX,
        r"export inline std::atomic<std::uint64_t> g_query_index_hit_total\{0\};",
        "3638 AC1: hit counter exported (evaluator.ixx)",
    ),
    (
        EVAL_IXX,
        r"export inline std::atomic<std::uint64_t> g_query_full_scan_fallback_total\{0\};",
        "3638 AC1: fallback counter exported (evaluator.ixx)",
    ),
    (QI, r"Issue\s+#3638", "3638 AC1: tag/arity probe cites #3638"),
    (QI, r"g_query_full_scan_fallback_total\.fetch_add", "3638 AC2: fallback bump at the probe"),
    (QI, r"g_query_index_hit_total\.fetch_add", "3638 AC2: hit bump at the probe"),
    (DEFUSE, r"Issue\s+#3638", "3638 AC1: defuse axis cites #3638"),
    (QPRIMS, r"query:index-hit-total", "3638 AC4: index-hit-total stats face"),
    (QPRIMS, r"query:full-scan-fallback-total", "3638 AC4: full-scan-fallback-total stats face"),
    (TEST, r"3638 AC2", "3638 AC2: test hosts cold-fallback / synced-hit face"),
    (TEST, r"3638 AC4", "3638 AC4: test reads the stats faces"),
    ("build.py", r"check_query_index_telemetry_3638", "3638 AC6: build.py wires the linter"),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def run_checks() -> list[str]:
    """Returns a list of failure labels (empty = clean)."""
    failures: list[str] = []
    cache: dict[str, str] = {}

    def body(path: str) -> str:
        if path not in cache:
            full = REPO_ROOT / path
            cache[path] = read(full) if full.exists() else ""
        return cache[path]

    for path, pattern, label in REQUIRED:
        if re.search(pattern, body(path)) is None:
            failures.append(label)

    qi = body(QI)

    # AC3 (shared-lock scope): the bucket probe takes the index lock across
    # build + by-value copy only — the lock is not held across match
    # materialization (the probe returns an owned vector).
    bucket_pos = qi.find("Evaluator::snapshot_tag_arity_bucket(")
    lock_pos = qi.find("wlock(tag_arity_index_mtx_)", bucket_pos)
    copy_pos = qi.find("const auto epoch_at_copy", lock_pos)
    if bucket_pos == -1 or lock_pos == -1 or copy_pos == -1:
        failures.append("3638 AC3: bucket probe lock-scope anchors not found")
    elif not (bucket_pos < lock_pos < copy_pos):
        failures.append("3638 AC3: lock must cover build + copy inside the probe only")

    # AC1 ordering: the hit/fallback bumps sit AFTER build_tag_arity_index_
    # unlocked in the bucket probe (post-decision, once per call).
    build_pos = qi.find("build_tag_arity_index_unlocked(trigger);", bucket_pos)
    bump_pos = qi.find("g_query_full_scan_fallback_total.fetch_add", build_pos)
    if build_pos == -1 or bump_pos == -1 or not (build_pos < bump_pos):
        failures.append("3638 AC1: fallback bump must follow the rebuild decision")

    # AC6: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3638-"):
                failures.append(f"3638 AC6: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3638.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3638.cpp",
    ):
        if probe.exists():
            failures.append(f"3638 AC6: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def self_test() -> int:
    """Regexes compile + anchor targets exist (pre-flight)."""
    ok = True
    for _, pattern, label in REQUIRED:
        try:
            re.compile(pattern)
        except re.error as e:
            print(f"SELF-TEST FAIL (regex compile): {label}: {e}")
            ok = False
    for path, _, _ in REQUIRED:
        if not (REPO_ROOT / path).exists():
            print(f"SELF-TEST FAIL (missing target): {path}")
            ok = False
    if ok:
        print("self-test: regexes compile + targets present")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3638 query index hit/fallback telemetry gate")
    ap.add_argument("--self-test", action="store_true", help="compile regexes + probe targets")
    ap.add_argument("--strict", action="store_true", help="exit non-zero on any failure")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    failures = run_checks()
    if failures:
        print(f"check_query_index_telemetry_3638: {len(failures)} failure(s)")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1 if args.strict else 0
    print("check_query_index_telemetry_3638: clean (index hit/fallback telemetry)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
