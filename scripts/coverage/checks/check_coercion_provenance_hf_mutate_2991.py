#!/usr/bin/env python3
"""Issue #2991: coercion provenance completeness under high-frequency mutate.

Session mid (explicit / engine / TLS) must beat log.back() and leftover
NarrowingRecords. check_flat / synthesize_flat_call pass explicit context.

Contract:
  AC1 session mid non-zero on deferred add / insert
  AC2 occurrence predicate attached
  AC3 multi-mutate does not steal later log.back()
  AC4 coercion_blame_chain_complete + missing metrics; schema-2991
  AC5 extend test_coercion_stamp_at_add; no docs/design / invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    cm = _read("src/compiler/coercion_map.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    tix = _read("src/compiler/type_checker.ixx")
    met = _read("src/compiler/observability_metrics.h")
    q = read_query_prims()
    t = _read("tests/compiler/test_coercion_stamp_at_add.cpp")
    build = _read("build.py")

    must("#2991", "AC1", cm)
    must("resolve_deferred_coercion_provenance", "AC1", cm)
    must("deferred_coercion_session_mid", "AC1", cm)
    must("g_coercion_blame_chain_complete_total", "AC1", cm)
    must("explicit_source_mutation_id", "AC1", impl)
    must("explicit_source_mutation_id", "AC1 decl", tix)

    must("engine_active_pred", "AC2", cm)
    must("last_predicate_cond_id_", "AC2", impl)
    must("ac2991_2_occurrence_predicate", "AC2", t)

    must("log_back_mid", "AC3", cm)
    must("leftover from an earlier/later mutate", "AC3", impl)
    must("ac2991_3_multi_mutate_loop", "AC3", t)

    must("coercion_blame_chain_complete_total", "AC4", met)
    must_key("schema-2991", "AC4", q)
    must_key("coercion-blame-chain-complete-total", "AC4", q)
    must_key("coercion-blame-missing-total", "AC4", q)
    must("ac2991_4_metrics_schema", "AC4", t)

    must("ac2991_1_session_beats_log_back", "AC5", t)
    must("ac2991_5_source_linter", "AC5", t)
    must("check_coercion_provenance_hf_mutate_2991", "AC5", build)
    must("cs_.active_mutation_id()", "AC5 pass explicit", impl)
    if (ROOT / "tests" / "compiler" / "test_issue_2991.cpp").is_file():
        fails.append("AC5: test_issue_2991.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2991-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2991 coercion provenance hf-mutate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
