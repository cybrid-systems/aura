#!/usr/bin/env python3
"""Issue #2989: query:pattern / where / filter default SafePCVSpan + hygiene.

Production concurrent query must:
  - pin children via children_columnar / children_safe (SafePCVSpan)
  - default-skip MacroIntroduced (opt-in include)
  - complete match under shared_lock or generation snapshot + retry
  - expose query:hygiene-skip-count + query:safe-span-pin-count

Extends test_query_pattern_default_hygiene + test_query_hygiene_default
(#81967). No docs/design/. Do not invent test_issue_2989.cpp or
test_edsl_query_concurrent_hygiene_safe_span.cpp.

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

    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    matcher = _read("src/compiler/query_matcher.ixx")
    mcpp = _read("src/compiler/query_matcher.cpp")
    eix = _read("src/compiler/evaluator.ixx")
    met = _read("src/compiler/observability_metrics.h")
    q = read_query_prims()
    t = _read("tests/compiler/test_query_pattern_default_hygiene.cpp")
    t2525 = _read("tests/compiler/test_query_hygiene_default.cpp")
    build = _read("build.py")

    # AC1 — production query prims default SafePCVSpan.
    must("#2989", "AC1", qws)
    must("pin_query_children", "AC1", qws)
    must("children_columnar", "AC1", qws)
    must("children_safe_view", "AC1 matcher", mcpp)
    must("bump_query_safe_span_pin", "AC1", eix)
    must("query_safe_span_pin_count", "AC1", met)

    # AC2 — default hygiene skip MacroIntroduced + opt-in.
    must("include_macro_introduced = false", "AC2", qws)
    must("hygiene_skip_macro = true", "AC2 filter default", qws)
    must("include-macro-introduced", "AC2", qws)
    must("skip_macro_introduced", "AC2 matcher", matcher + mcpp)
    must("#2989", "AC2 matcher cite", matcher + mcpp)

    # AC3 — shared_lock + generation snapshot / retry.
    must("shared_lock", "AC3", qws)
    must("kQueryEpochAttempts", "AC3 filter retry", qws)
    must("bump_query_epoch_retry", "AC3", qws)
    must("query_epoch_retry_total", "AC3", met)

    # AC4 — Agent counters.
    must_key("query:hygiene-skip-count", "AC4", qws + q)
    must_key("query:safe-span-pin-count", "AC4", qws + q)
    must("get_query_hygiene_skip_count", "AC4", eix)
    must("get_query_safe_span_pin_count", "AC4", eix)
    must_key("schema-2989", "AC4", q)
    must_key("hygiene-skip-count", "AC4 stats", q)
    must_key("safe-span-pin-count", "AC4 stats", q)

    # AC5 — extend existing suite + concurrent mutate.
    must("ac2989_1_safe_span_default", "AC5", t)
    must("ac2989_2_default_hygiene", "AC5", t)
    must("ac2989_3_metrics", "AC5", t)
    must("ac2989_4_concurrent_query_mutate", "AC5", t)
    must("ac2989_5_observability", "AC5", t)
    must("ac2989_6_source_and_linter", "AC5", t)
    must("ac2989_concurrent_mutate", "AC5 #2525 suite", t2525)
    must("mutate:rebind", "AC5 concurrent mutate", t)

    # AC6 — linter wired; no invented tests / design docs.
    must("check_query_concurrent_hygiene_safe_span_2989", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2989.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2989.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "test_edsl_query_concurrent_hygiene_safe_span.cpp").is_file():
        fails.append(
            "AC6: tests/test_edsl_query_concurrent_hygiene_safe_span.cpp present (extend existing suite per #81967)"
        )
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2989-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2989 query concurrent SafePCVSpan + hygiene default — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
