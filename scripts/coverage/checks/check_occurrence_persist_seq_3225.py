#!/usr/bin/env python3
"""Issue #3225: occurrence persist seqlock vs concurrent rehydrate.

Outermost persist write and densify/steal rehydrate share the persist
side-buffer with no generation. A torn copy can freeze a mixed
fingerprint. Production seqlock: write odd→even; rehydrate miss on odd
or gen change. Soft/quiet skip seq. Reuse miss + empty-after-fence.

Contract:
  AC1 Production in-flight persist vs rehydrate → miss, empty, no green
  AC2 last_proof_goal_fingerprint / live_goal_count never mixed
  AC3 Soft / quiet zero extra seq
  AC4 Extend persist-rehydrate suite; linter; no invent / docs/design;
      no new g_3225_* / query:*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    impl = _read("src/compiler/type_checker_impl.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    must("kOccurrencePersistSeqIssue", "AC1 stamp", tma)
    must("g_occurrence_persist_seq", "AC1 seq", tma)
    must("occurrence_persist_seq_begin_write", "AC1 begin", tma)
    must("occurrence_persist_seq_end_write", "AC1 end", impl)
    must("Issue #3225", "AC1 append cite", impl)
    must("g0 & 1ull", "AC1 odd in-flight", impl)
    must("g0 != g1", "AC1 mid-copy", impl)
    must("Issue #3225", "AC1 persist helper", mb)
    must("invalidate_fast_path_on_rehydrate_miss", "AC1 miss invalidate", impl)
    must("note_occurrence_empty_after_fence", "AC1 miss face", impl)

    must("last_proof_goal_fingerprint_v_read", "AC2", t)
    must("ac3225_occurrence_persist_seqlock", "AC2 test", t)

    must("if (occurrence_persist_seq_hard())", "AC3 Soft skip", tma)
    must("ac3225_occurrence_persist_seqlock", "AC3 test", t)

    must("occurrence_persist_rehydrate_miss_total", "AC4 reuse miss", impl)
    must("check_occurrence_persist_seq_3225", "AC4 build.py", build)
    if "g_3225_" in tma or "g_3225_" in impl:
        fails.append("AC4: new g_3225_* counter")
    if "query:occurrence-persist-seq" in tma:
        fails.append("AC4: new query:*")

    if (ROOT / "tests" / "issues" / "test_issue_3225.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3225.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3225.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3225.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3225-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3225 occurrence_persist_seq:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3225 occurrence_persist_seq: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
